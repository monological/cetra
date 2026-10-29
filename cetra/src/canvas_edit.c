#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas_record.h"
#include "ext/log.h"
#include "text.h"
#include "triangulate.h"

// The most pieces one shape is split into. Past it the shape reaches across
// more of the plane than any drawing needs, and splitting it would be an
// allocation the size of a mistake.
#define CANVAS_SPLIT_MAX 65536

static const double TURN = 6.283185307179586;

CanvasEdit* create_canvas_edit(const Canvas* canvas) {
    if (!canvas) {
        log_error("canvas: an edit needs the canvas it is for");
        return NULL;
    }
    CanvasEdit* edit = calloc(1, sizeof(CanvasEdit));
    if (!edit) {
        log_error("canvas: out of memory making an edit");
        return NULL;
    }
    edit->tile_shift = canvas_tile_shift(canvas);
    return edit;
}

void free_canvas_edit(CanvasEdit* edit) {
    if (!edit)
        return;
    free(edit->pieces);
    free(edit->fills);
    free(edit->steps);
    free(edit);
}

static bool _grow(void** items, size_t* cap, size_t want, size_t size) {
    if (want <= *cap)
        return true;
    size_t next = *cap ? *cap * 2 : 64;
    while (next < want)
        next *= 2;
    void* grown = realloc(*items, next * size);
    if (!grown) {
        log_error("canvas: out of memory growing an edit");
        return false;
    }
    *items = grown;
    *cap = next;
    return true;
}

static bool _step(CanvasEdit* edit, CanvasStep step) {
    if (!_grow((void**)&edit->steps, &edit->step_cap, edit->step_count + 1, sizeof(CanvasStep)))
        return false;
    edit->steps[edit->step_count++] = step;
    return true;
}

// The pieces of one shape go in together or not at all: reserved first, then
// filled, then recorded as one step.
static CanvasPiece* _reserve(CanvasEdit* edit, size_t count) {
    if (!_grow((void**)&edit->pieces, &edit->piece_cap, edit->piece_count + count,
               sizeof(CanvasPiece)))
        return NULL;
    return edit->pieces + edit->piece_count;
}

static void _commit(CanvasEdit* edit, size_t count) {
    CanvasStep* last = edit->step_count ? &edit->steps[edit->step_count - 1] : NULL;
    if (last && last->kind == CANVAS_STEP_ADD && last->first + last->count == edit->piece_count) {
        last->count += count;
        edit->piece_count += count;
        return;
    }
    if (_step(edit,
              (CanvasStep){.kind = CANVAS_STEP_ADD, .first = edit->piece_count, .count = count}))
        edit->piece_count += count;
}

// A fill triangle, added at once: a polygon's triangles are many and each is
// cut on its own, so they are not reserved together.
static void _add_fill(CanvasEdit* edit, const CanvasFillPiece* piece) {
    if (!_grow((void**)&edit->fills, &edit->fill_cap, edit->fill_count + 1,
               sizeof(CanvasFillPiece)))
        return;
    edit->fills[edit->fill_count] = *piece;
    CanvasStep* last = edit->step_count ? &edit->steps[edit->step_count - 1] : NULL;
    if (last && last->kind == CANVAS_STEP_ADD_FILLS &&
        last->first + last->count == edit->fill_count) {
        last->count++;
        edit->fill_count++;
        return;
    }
    if (_step(edit,
              (CanvasStep){.kind = CANVAS_STEP_ADD_FILLS, .first = edit->fill_count, .count = 1}))
        edit->fill_count++;
}

// The tile holding a unit: its index, floored, so -1 is in tile -1.
static int64_t _tile_of(int64_t v, int shift) {
    return v >= 0 ? v >> shift : -(((-(v + 1)) >> shift) + 1);
}

static int64_t _tile_origin(int64_t tile, int shift) {
    return tile * ((int64_t)1 << shift);
}

// Starts a piece anchored in the tile of `anchor`, with its paint.
static CanvasPiece* _piece(CanvasPiece* piece, int shift, const CanvasPaint* paint,
                           CanvasPoint anchor, uint32_t kind, uint32_t flags) {
    memset(piece, 0, sizeof(*piece));
    piece->layer = paint->layer;
    piece->tx = _tile_of(anchor.x, shift);
    piece->ty = _tile_of(anchor.y, shift);
    piece->record.u[0] = canvas_record_head(kind, flags, paint->style);
    piece->record.u[1] = paint->net;
    piece->record.u[2] = paint->object;
    return piece;
}

static int32_t _rel(int64_t v, int64_t tile, int shift) {
    return (int32_t)(v - _tile_origin(tile, shift));
}

static void _bound(CanvasPiece* piece, int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                   int64_t grow) {
    piece->min[0] = (x0 < x1 ? x0 : x1) - grow;
    piece->min[1] = (y0 < y1 ? y0 : y1) - grow;
    piece->max[0] = (x0 > x1 ? x0 : x1) + grow;
    piece->max[1] = (y0 > y1 ? y0 : y1) + grow;
}

static bool _paint_ok(const CanvasEdit* edit, const CanvasPaint* paint, const char* what) {
    if (!edit)
        return false;
    if (!paint->layer) {
        log_error("canvas: a %s with no layer is refused", what);
        return false;
    }
    return true;
}

static bool _length_ok(int64_t v, const char* what, const char* field) {
    if (v < 0 || v >= CANVAS_LENGTH_MAX) {
        log_error("canvas: a %s whose %s is %lld is refused (0 to %lld)", what, field, (long long)v,
                  (long long)CANVAS_LENGTH_MAX - 1);
        return false;
    }
    return true;
}

static int64_t _abs64(int64_t v) {
    return v < 0 ? -v : v;
}

// How many pieces keep each within a tile of its first point, or 0 past the
// limit.
static size_t _pieces_for(double span, int shift) {
    double n = ceil(span / (double)((int64_t)1 << shift));
    if (n > CANVAS_SPLIT_MAX)
        return 0;
    return n < 1.0 ? 1 : (size_t)n;
}

static bool _segment(CanvasEdit* edit, const CanvasPaint* paint, CanvasPoint a, CanvasPoint b,
                     int64_t width, bool flat_ends) {
    const int shift = edit->tile_shift;
    const int64_t dx = b.x - a.x, dy = b.y - a.y;
    const size_t n =
        _pieces_for((double)(_abs64(dx) > _abs64(dy) ? _abs64(dx) : _abs64(dy)), shift);
    if (n == 0) {
        log_error("canvas: a segment across more than %d tiles is refused", CANVAS_SPLIT_MAX);
        return false;
    }
    CanvasPiece* pieces = _reserve(edit, n);
    if (!pieces)
        return false;
    const int64_t half = (width + 1) / 2;
    CanvasPoint from = a;
    for (size_t k = 0; k < n; k++) {
        // The points between pieces are rounded onto the unit grid, so a split
        // segment bends by under half a unit where it was cut.
        CanvasPoint to = b;
        if (k + 1 < n) {
            const double t = (double)(k + 1) / (double)n;
            to.x = a.x + llround((double)dx * t);
            to.y = a.y + llround((double)dy * t);
        }
        uint32_t flags = 0;
        flags |= k == 0 ? (flat_ends ? CANVAS_FLAG_FLAT_A : 0) : CANVAS_FLAG_CUT_A;
        flags |= k + 1 == n ? (flat_ends ? CANVAS_FLAG_FLAT_B : 0) : CANVAS_FLAG_CUT_B;
        CanvasPiece* piece = _piece(&pieces[k], shift, paint, from, CANVAS_KIND_SEGMENT, flags);
        piece->record.p[0] = _rel(from.x, piece->tx, shift);
        piece->record.p[1] = _rel(from.y, piece->ty, shift);
        piece->record.p[2] = _rel(to.x, piece->tx, shift);
        piece->record.p[3] = _rel(to.y, piece->ty, shift);
        piece->record.f[0] = (float)((double)width * 0.5);
        _bound(piece, from.x, from.y, to.x, to.y, half);
        from = to;
    }
    _commit(edit, n);
    return true;
}

void canvas_edit_segment(CanvasEdit* edit, const CanvasSegment* segment) {
    if (!segment || !_paint_ok(edit, &segment->paint, "segment"))
        return;
    if (!_length_ok(segment->width, "segment", "width"))
        return;
    if (segment->a.x == segment->b.x && segment->a.y == segment->b.y &&
        (segment->width == 0 || segment->flat_ends)) {
        log_error("canvas: a segment with no extent is refused");
        return;
    }
    _segment(edit, &segment->paint, segment->a, segment->b, segment->width, segment->flat_ends);
}

void canvas_edit_polyline(CanvasEdit* edit, const CanvasPolyline* line) {
    if (!line || !_paint_ok(edit, &line->paint, "polyline"))
        return;
    if (!line->points || line->count < 2) {
        log_error("canvas: a polyline of fewer than two points is refused");
        return;
    }
    if (!_length_ok(line->width, "polyline", "width"))
        return;
    if (line->width == 0) {
        log_error("canvas: a polyline with no width is refused");
        return;
    }
    const size_t segments = line->closed ? line->count : line->count - 1;
    for (size_t i = 0; i < segments; i++)
        if (!_segment(edit, &line->paint, line->points[i], line->points[(i + 1) % line->count],
                      line->width, false))
            return;
}

// Puts the bound of a circular arc into min/max: its ends, and wherever it
// passes a quarter turn of its circle.
static void _arc_bound(double cx, double cy, double r, double start, double sweep, double out[4]) {
    out[0] = out[2] = cx + r * cos(start);
    out[1] = out[3] = cy + r * sin(start);
    const double end = start + sweep;
    const double ex = cx + r * cos(end), ey = cy + r * sin(end);
    out[0] = fmin(out[0], ex), out[2] = fmax(out[2], ex);
    out[1] = fmin(out[1], ey), out[3] = fmax(out[3], ey);
    const double lo = fmin(start, end), hi = fmax(start, end);
    for (double q = ceil(lo / (TURN / 4.0)) * (TURN / 4.0); q < hi; q += TURN / 4.0) {
        const double x = cx + r * cos(q), y = cy + r * sin(q);
        out[0] = fmin(out[0], x), out[2] = fmax(out[2], x);
        out[1] = fmin(out[1], y), out[3] = fmax(out[3], y);
    }
}

void canvas_edit_arc(CanvasEdit* edit, const CanvasArc* arc) {
    if (!arc || !_paint_ok(edit, &arc->paint, "arc"))
        return;
    if (!_length_ok(arc->width, "arc", "width") || !_length_ok(arc->radius, "arc", "radius"))
        return;
    const double ax = (double)arc->a.x, ay = (double)arc->a.y;
    const double bx = (double)arc->b.x, by = (double)arc->b.y;
    const double chord = hypot(bx - ax, by - ay);
    if (chord == 0.0) {
        log_error("canvas: an arc from a point to itself is refused");
        return;
    }
    // The centre is left of the chord for a counter-clockwise arc the short
    // way, and for a clockwise one the long way.
    const double r = fmax((double)arc->radius, chord * 0.5);
    const double h = sqrt(fmax(r * r - chord * chord * 0.25, 0.0));
    const double side = arc->clockwise == arc->major ? 1.0 : -1.0;
    const double cx = (ax + bx) * 0.5 - side * h * (by - ay) / chord;
    const double cy = (ay + by) * 0.5 + side * h * (bx - ax) / chord;
    const double start = atan2(ay - cy, ax - cx);
    double sweep = atan2(by - cy, bx - cx) - start;
    if (arc->clockwise) {
        sweep = -sweep;
        while (sweep <= 0.0)
            sweep += TURN;
        sweep = -sweep; // signed: negative runs clockwise
    } else {
        while (sweep <= 0.0)
            sweep += TURN;
    }

    // Split only an arc that reaches past its first point's tile, into pieces
    // of at most a quarter turn whose chords are no longer than a tile.
    const int shift = edit->tile_shift;
    const double tile = (double)((int64_t)1 << shift);
    double whole[4];
    _arc_bound(cx, cy, r, start, sweep, whole);
    const double ox = (double)_tile_origin(_tile_of(arc->a.x, shift), shift);
    const double oy = (double)_tile_origin(_tile_of(arc->a.y, shift), shift);
    const bool fits = whole[0] >= ox - tile && whole[2] < ox + 2.0 * tile &&
                      whole[1] >= oy - tile && whole[3] < oy + 2.0 * tile;
    size_t n = 1;
    if (!fits) {
        const double by_turn = ceil(fabs(sweep) / (TURN / 4.0));
        const double by_length = ceil(r * fabs(sweep) / tile);
        const double want = fmax(by_turn, by_length);
        if (want > CANVAS_SPLIT_MAX) {
            log_error("canvas: an arc across more than %d tiles is refused", CANVAS_SPLIT_MAX);
            return;
        }
        n = (size_t)want;
    }
    CanvasPiece* pieces = _reserve(edit, n);
    if (!pieces)
        return;
    const int64_t half = (arc->width + 1) / 2;
    CanvasPoint from = arc->a;
    for (size_t k = 0; k < n; k++) {
        CanvasPoint to = arc->b;
        if (k + 1 < n) {
            const double t = start + sweep * (double)(k + 1) / (double)n;
            to.x = llround(cx + r * cos(t));
            to.y = llround(cy + r * sin(t));
        }
        uint32_t flags = arc->clockwise ? CANVAS_FLAG_CLOCKWISE : 0;
        flags |= k == 0 ? (arc->flat_ends ? CANVAS_FLAG_FLAT_A : 0) : CANVAS_FLAG_CUT_A;
        flags |= k + 1 == n ? (arc->flat_ends ? CANVAS_FLAG_FLAT_B : 0) : CANVAS_FLAG_CUT_B;
        CanvasPiece* piece = _piece(&pieces[k], shift, &arc->paint, from, CANVAS_KIND_ARC, flags);
        piece->record.p[0] = _rel(from.x, piece->tx, shift);
        piece->record.p[1] = _rel(from.y, piece->ty, shift);
        piece->record.p[2] = _rel(to.x, piece->tx, shift);
        piece->record.p[3] = _rel(to.y, piece->ty, shift);
        // The centre from the midpoint of this piece's own chord, which keeps
        // the float small however far the canvas is from its origin.
        const double mx = ((double)from.x + (double)to.x) * 0.5;
        const double my = ((double)from.y + (double)to.y) * 0.5;
        piece->record.f[0] = (float)((double)arc->width * 0.5);
        piece->record.f[1] = (float)(cx - mx);
        piece->record.f[2] = (float)(cy - my);
        piece->record.f[3] = (float)r;
        double b[4];
        _arc_bound(cx, cy, r, start + sweep * (double)k / (double)n, sweep / (double)n, b);
        _bound(piece, (int64_t)floor(b[0]), (int64_t)floor(b[1]), (int64_t)ceil(b[2]),
               (int64_t)ceil(b[3]), half);
        from = to;
    }
    _commit(edit, n);
}

void canvas_edit_circle(CanvasEdit* edit, const CanvasCircle* circle) {
    if (!circle || !_paint_ok(edit, &circle->paint, "circle"))
        return;
    if (!_length_ok(circle->radius, "circle", "radius") ||
        !_length_ok(circle->stroke, "circle", "stroke"))
        return;
    if (circle->radius == 0) {
        log_error("canvas: a circle with no radius is refused");
        return;
    }
    CanvasPiece* piece = _reserve(edit, 1);
    if (!piece)
        return;
    const int shift = edit->tile_shift;
    _piece(piece, shift, &circle->paint, circle->centre, CANVAS_KIND_CIRCLE, 0);
    piece->record.p[0] = _rel(circle->centre.x, piece->tx, shift);
    piece->record.p[1] = _rel(circle->centre.y, piece->ty, shift);
    piece->record.p[2] = (int32_t)circle->radius;
    piece->record.p[3] = (int32_t)circle->stroke;
    const int64_t reach = circle->radius + (circle->stroke + 1) / 2;
    _bound(piece, circle->centre.x, circle->centre.y, circle->centre.x, circle->centre.y, reach);
    _commit(edit, 1);
}

void canvas_edit_box(CanvasEdit* edit, const CanvasBox* box) {
    if (!box || !_paint_ok(edit, &box->paint, "box"))
        return;
    if (!_length_ok(box->size_x, "box", "size_x") || !_length_ok(box->size_y, "box", "size_y") ||
        !_length_ok(box->corner, "box", "corner") || !_length_ok(box->stroke, "box", "stroke"))
        return;
    if (box->size_x == 0 || box->size_y == 0) {
        log_error("canvas: a box with no area is refused");
        return;
    }
    CanvasPiece* piece = _reserve(edit, 1);
    if (!piece)
        return;
    const int shift = edit->tile_shift;
    _piece(piece, shift, &box->paint, box->centre, CANVAS_KIND_BOX, 0);
    piece->record.p[0] = _rel(box->centre.x, piece->tx, shift);
    piece->record.p[1] = _rel(box->centre.y, piece->ty, shift);
    piece->record.p[2] = (int32_t)box->stroke;
    piece->record.f[0] = (float)((double)box->size_x * 0.5);
    piece->record.f[1] = (float)((double)box->size_y * 0.5);
    piece->record.f[2] = (float)box->corner;
    piece->record.f[3] = (float)box->angle;
    const double c = fabs(cos(box->angle)), s = fabs(sin(box->angle));
    const double hx = (double)box->size_x * 0.5, hy = (double)box->size_y * 0.5;
    const int64_t ex = (int64_t)ceil(c * hx + s * hy) + (box->stroke + 1) / 2;
    const int64_t ey = (int64_t)ceil(s * hx + c * hy) + (box->stroke + 1) / 2;
    piece->min[0] = box->centre.x - ex, piece->max[0] = box->centre.x + ex;
    piece->min[1] = box->centre.y - ey, piece->max[1] = box->centre.y + ey;
    _commit(edit, 1);
}

/*
 * Polygons. A filled polygon is its triangles and its outline's outer ramp:
 * the triangles carry the inner half of the ramp on their outline edges, and
 * EDGE records the outer half. Anything reaching past a tile is cut at the
 * tile grid, and every point where an edge crosses a grid line is found from
 * the edge's two ends taken in one order: the triangles either side of a grid
 * line, and the edge records along it, then share their corners to the unit,
 * so the pieces meet with no crack.
 */

// Where the line through a and b crosses x = at (axis 0) or y = at (axis 1),
// rounded onto the unit grid.
static CanvasPoint _crossing(CanvasPoint a, CanvasPoint b, int axis, int64_t at) {
    if (b.x < a.x || (b.x == a.x && b.y < a.y)) {
        const CanvasPoint t = a;
        a = b;
        b = t;
    }
    if (axis == 0) {
        const double t = (double)(at - a.x) / (double)(b.x - a.x);
        return (CanvasPoint){at, a.y + llround(t * (double)(b.y - a.y))};
    }
    const double t = (double)(at - a.y) / (double)(b.y - a.y);
    return (CanvasPoint){a.x + llround(t * (double)(b.x - a.x)), at};
}

// A corner of a triangle being clipped, and the line the edge arriving at it
// lies on: 0 to 2 the triangle's own edges (k from corner k to k + 1), 3 and 4
// the tile's left and right sides, 5 and 6 its bottom and top.
typedef struct ClipCorner {
    CanvasPoint p;
    int line;
} ClipCorner;

#define CLIP_MAX 12 // a triangle clipped by four lines has at most seven corners

typedef struct ClipBox {
    int64_t x0, x1, y0, y1;
} ClipBox;

static bool _clip_inside(CanvasPoint p, int line, const ClipBox* box) {
    switch (line) {
        case 3:
            return p.x >= box->x0;
        case 4:
            return p.x <= box->x1;
        case 5:
            return p.y >= box->y0;
        default:
            return p.y <= box->y1;
    }
}

// Where the edge on `line` crosses the tile side `side`: a triangle edge from
// its ends, or two of the tile's sides at their corner.
static CanvasPoint _clip_cross(const CanvasPoint tri[3], int line, int side, const ClipBox* box) {
    const int64_t at = side == 3 ? box->x0 : side == 4 ? box->x1 : side == 5 ? box->y0 : box->y1;
    if (line < 3)
        return _crossing(tri[line], tri[(line + 1) % 3], side <= 4 ? 0 : 1, at);
    const int64_t x = line == 3 ? box->x0 : line == 4 ? box->x1 : at;
    const int64_t y = line == 5 ? box->y0 : line == 6 ? box->y1 : at;
    return (CanvasPoint){x, y};
}

// Sutherland-Hodgman against the tile's four sides in turn.
static int _clip_triangle(const CanvasPoint tri[3], const ClipBox* box, ClipCorner out[CLIP_MAX]) {
    ClipCorner a[CLIP_MAX], b[CLIP_MAX];
    a[0] = (ClipCorner){tri[0], 2};
    a[1] = (ClipCorner){tri[1], 0};
    a[2] = (ClipCorner){tri[2], 1};
    int n = 3;
    ClipCorner* in = a;
    ClipCorner* next = b;
    for (int side = 3; side <= 6 && n > 0; side++) {
        int m = 0;
        for (int j = 0; j < n && m + 2 <= CLIP_MAX; j++) {
            const ClipCorner* p = &in[(j + n - 1) % n];
            const ClipCorner* q = &in[j];
            const bool p_in = _clip_inside(p->p, side, box), q_in = _clip_inside(q->p, side, box);
            if (p_in && q_in) {
                next[m++] = *q;
            } else if (p_in) {
                next[m++] = (ClipCorner){_clip_cross(tri, q->line, side, box), q->line};
            } else if (q_in) {
                next[m++] = (ClipCorner){_clip_cross(tri, q->line, side, box), side};
                next[m++] = *q;
            }
        }
        n = m;
        ClipCorner* t = in;
        in = next;
        next = t;
    }
    memcpy(out, in, (size_t)n * sizeof(ClipCorner));
    return n;
}

static int64_t _cross3(CanvasPoint a, CanvasPoint b, CanvasPoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// A fill triangle in the tile (tx, ty), its corners given, and the nine
// outline points its inner ramp is measured against (canvas_record.h).
// Corners that rounded onto a line add nothing.
static void _fill(CanvasEdit* edit, const CanvasPaint* paint, int64_t tx, int64_t ty,
                  const CanvasPoint c[3], const CanvasPoint near[9]) {
    if (_cross3(c[0], c[1], c[2]) == 0)
        return;
    const int shift = edit->tile_shift;
    CanvasFillPiece piece = {.layer = paint->layer, .tx = tx, .ty = ty};
    piece.min[0] = piece.max[0] = c[0].x;
    piece.min[1] = piece.max[1] = c[0].y;
    const uint32_t head = canvas_record_head(CANVAS_KIND_FILL, 0, paint->style);
    for (int k = 0; k < 3; k++) {
        CanvasFillVertex* v = &piece.triangle.v[k];
        v->p[0] = _rel(c[k].x, tx, shift);
        v->p[1] = _rel(c[k].y, ty, shift);
        for (int n = 0; n < 9; n++) {
            v->near[2 * n] = _rel(near[n].x, tx, shift);
            v->near[2 * n + 1] = _rel(near[n].y, ty, shift);
        }
        v->u[0] = head, v->u[1] = paint->net, v->u[2] = paint->object;
        piece.min[0] = c[k].x < piece.min[0] ? c[k].x : piece.min[0];
        piece.min[1] = c[k].y < piece.min[1] ? c[k].y : piece.min[1];
        piece.max[0] = c[k].x > piece.max[0] ? c[k].x : piece.max[0];
        piece.max[1] = c[k].y > piece.max[1] ? c[k].y : piece.max[1];
    }
    _add_fill(edit, &piece);
}

// One triangle of a polygon: whole when it fits in two tiles each way of its
// lowest, else cut into each tile it crosses, each cut piece a fan. Every
// piece carries the whole triangle's nine outline points.
static bool _polygon_triangle(CanvasEdit* edit, const CanvasPaint* paint, const CanvasPoint tri[3],
                              const CanvasPoint near[9]) {
    const int shift = edit->tile_shift;
    int64_t x0 = tri[0].x, y0 = tri[0].y;
    int64_t x1 = x0, y1 = y0;
    for (int k = 1; k < 3; k++) {
        x0 = tri[k].x < x0 ? tri[k].x : x0, x1 = tri[k].x > x1 ? tri[k].x : x1;
        y0 = tri[k].y < y0 ? tri[k].y : y0, y1 = tri[k].y > y1 ? tri[k].y : y1;
    }
    const int64_t tx0 = _tile_of(x0, shift), tx1 = _tile_of(x1, shift);
    const int64_t ty0 = _tile_of(y0, shift), ty1 = _tile_of(y1, shift);
    if (tx1 - tx0 <= 1 && ty1 - ty0 <= 1) {
        _fill(edit, paint, tx0, ty0, tri, near);
        return true;
    }
    if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) > CANVAS_SPLIT_MAX) {
        log_error("canvas: a polygon triangle across more than %d tiles is refused",
                  CANVAS_SPLIT_MAX);
        return false;
    }
    for (int64_t ty = ty0; ty <= ty1; ty++) {
        for (int64_t tx = tx0; tx <= tx1; tx++) {
            const ClipBox box = {_tile_origin(tx, shift), _tile_origin(tx + 1, shift),
                                 _tile_origin(ty, shift), _tile_origin(ty + 1, shift)};
            ClipCorner c[CLIP_MAX];
            const int n = _clip_triangle(tri, &box, c);
            for (int k = 1; k + 1 < n; k++) {
                const CanvasPoint fan[3] = {c[0].p, c[k].p, c[k + 1].p};
                _fill(edit, paint, tx, ty, fan, near);
            }
        }
    }
    return true;
}

// The outer half of an outline edge's ramp from a to b, cut where it crosses
// the tile grid at the points the triangles were cut at.
static bool _polygon_edge(CanvasEdit* edit, const CanvasPaint* paint, CanvasPoint a, CanvasPoint b,
                          bool inside_left) {
    const int shift = edit->tile_shift;
    const int64_t side = (int64_t)1 << shift;
    const int64_t ta[2] = {_tile_of(a.x, shift), _tile_of(a.y, shift)};
    const int64_t tb[2] = {_tile_of(b.x, shift), _tile_of(b.y, shift)};
    const int64_t lines = _abs64(tb[0] - ta[0]) + _abs64(tb[1] - ta[1]);
    if (lines + 1 > CANVAS_SPLIT_MAX) {
        log_error("canvas: a polygon edge across more than %d tiles is refused", CANVAS_SPLIT_MAX);
        return false;
    }
    // Every crossing with its place along the edge, then sorted along it.
    typedef struct Cut {
        double t;
        CanvasPoint p;
    } Cut;
    Cut* cuts = malloc((size_t)(lines + 2) * sizeof(Cut));
    if (!cuts)
        return false;
    size_t n = 0;
    cuts[n++] = (Cut){0.0, a};
    const double dx = (double)(b.x - a.x), dy = (double)(b.y - a.y);
    for (int axis = 0; axis < 2; axis++) {
        const int64_t lo = ta[axis] < tb[axis] ? ta[axis] : tb[axis];
        const int64_t hi = ta[axis] < tb[axis] ? tb[axis] : ta[axis];
        for (int64_t k = lo + 1; k <= hi; k++) {
            const int64_t at = k * side;
            const double t = axis == 0 ? ((double)(at - a.x)) / dx : ((double)(at - a.y)) / dy;
            cuts[n++] = (Cut){t, _crossing(a, b, axis, at)};
        }
    }
    cuts[n++] = (Cut){1.0, b};
    for (size_t i = 1; i < n; i++) // few, and nearly in order already
        for (size_t j = i; j > 0 && cuts[j - 1].t > cuts[j].t; j--) {
            const Cut t = cuts[j];
            cuts[j] = cuts[j - 1];
            cuts[j - 1] = t;
        }
    CanvasPiece* pieces = _reserve(edit, n - 1);
    if (!pieces) {
        free(cuts);
        return false;
    }
    size_t made = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        const CanvasPoint p = cuts[i].p, q = cuts[i + 1].p;
        if (p.x == q.x && p.y == q.y)
            continue;
        uint32_t flags = inside_left ? CANVAS_FLAG_INSIDE_LEFT : 0;
        flags |= i > 0 ? CANVAS_FLAG_CUT_A : 0;
        flags |= i + 2 < n ? CANVAS_FLAG_CUT_B : 0;
        // Anchored where its middle is, so both ends are within a tile of it.
        const CanvasPoint mid = {p.x + (q.x - p.x) / 2, p.y + (q.y - p.y) / 2};
        CanvasPiece* piece = _piece(&pieces[made++], shift, paint, mid, CANVAS_KIND_EDGE, flags);
        piece->record.p[0] = _rel(p.x, piece->tx, shift);
        piece->record.p[1] = _rel(p.y, piece->ty, shift);
        piece->record.p[2] = _rel(q.x, piece->tx, shift);
        piece->record.p[3] = _rel(q.y, piece->ty, shift);
        _bound(piece, p.x, p.y, q.x, q.y, 1);
    }
    free(cuts);
    _commit(edit, made);
    return true;
}

// The point after (or before) point i on its own ring.
static size_t _ring_step(const CanvasPolygon* polygon, size_t i, bool forward) {
    size_t start = 0, end = polygon->hole_count ? polygon->holes[0] : polygon->count;
    for (size_t h = 0; h < polygon->hole_count && i >= end; h++) {
        start = polygon->holes[h];
        end = h + 1 < polygon->hole_count ? polygon->holes[h + 1] : polygon->count;
    }
    if (forward)
        return i + 1 < end ? i + 1 : start;
    return i > start ? i - 1 : end - 1;
}

static bool _rings_ok(const CanvasPolygon* polygon) {
    const size_t outer = polygon->hole_count ? polygon->holes[0] : polygon->count;
    if (!polygon->points || outer < 3)
        return false;
    for (size_t h = 0; h < polygon->hole_count; h++) {
        const size_t end = h + 1 < polygon->hole_count ? polygon->holes[h + 1] : polygon->count;
        if (polygon->holes[h] < outer || end < polygon->holes[h] + 3 || end > polygon->count)
            return false;
    }
    return true;
}

void canvas_edit_polygon(CanvasEdit* edit, const CanvasPolygon* polygon) {
    if (!polygon || !_paint_ok(edit, &polygon->paint, "polygon"))
        return;
    if (!_length_ok(polygon->stroke, "polygon", "stroke"))
        return;
    if (!_rings_ok(polygon)) {
        log_error("canvas: a polygon whose outline or a hole has fewer than three points is "
                  "refused");
        return;
    }
    const CanvasPoint* pts = polygon->points;
    const size_t rings = polygon->hole_count + 1;
    if (polygon->stroke > 0) {
        for (size_t r = 0; r < rings; r++) {
            const size_t start = r ? polygon->holes[r - 1] : 0;
            const size_t end = r < polygon->hole_count ? polygon->holes[r] : polygon->count;
            canvas_edit_polyline(edit, &(CanvasPolyline){.paint = polygon->paint,
                                                         .points = pts + start,
                                                         .count = end - start,
                                                         .width = polygon->stroke,
                                                         .closed = true});
        }
        return;
    }

    // Triangulated about the first point, so the doubles stay small however
    // far the polygon is from the origin.
    double* xy = malloc(polygon->count * 2 * sizeof(double));
    if (!xy) {
        log_error("canvas: out of memory triangulating a polygon");
        return;
    }
    for (size_t i = 0; i < polygon->count; i++) {
        xy[2 * i] = (double)(pts[i].x - pts[0].x);
        xy[2 * i + 1] = (double)(pts[i].y - pts[0].y);
    }
    Triangulation tris;
    const bool triangulated =
        triangulate_polygon(xy, polygon->count, polygon->holes, polygon->hole_count, &tris);
    if (!triangulated || tris.count == 0) {
        log_error("canvas: a polygon of %zu points gave no triangles", polygon->count);
        free(xy);
        free_triangulation(&tris);
        return;
    }
    for (size_t t = 0; t < tris.count; t++) {
        const uint32_t* v = tris.indices + t * 3;
        const CanvasPoint tri[3] = {pts[v[0]], pts[v[1]], pts[v[2]]};
        CanvasPoint near[9] = {tri[0], tri[1], tri[2]};
        for (int k = 0; k < 3; k++) {
            near[3 + 2 * k] = pts[_ring_step(polygon, v[k], false)];
            near[4 + 2 * k] = pts[_ring_step(polygon, v[k], true)];
        }
        if (!_polygon_triangle(edit, &polygon->paint, tri, near))
            break;
    }
    free_triangulation(&tris);

    // The outer ramp: the polygon lies left of an outline wound
    // counter-clockwise, and outside a hole, so left of one wound clockwise.
    for (size_t r = 0; r < rings; r++) {
        const size_t start = r ? polygon->holes[r - 1] : 0;
        const size_t end = r < polygon->hole_count ? polygon->holes[r] : polygon->count;
        double area = 0.0;
        for (size_t i = start, j = end - 1; i < end; j = i++)
            area += xy[2 * j] * xy[2 * i + 1] - xy[2 * i] * xy[2 * j + 1];
        const bool inside_left = r == 0 ? area > 0.0 : area < 0.0;
        for (size_t i = start; i < end; i++) {
            const CanvasPoint a = pts[i], b = pts[i + 1 < end ? i + 1 : start];
            if ((a.x != b.x || a.y != b.y) &&
                !_polygon_edge(edit, &polygon->paint, a, b, inside_left)) {
                free(xy);
                return;
            }
        }
    }
    free(xy);
}

/*
 * Text. A string is a record a glyph, each placed by its pen from the string's
 * anchor in ems, with the em and the angle on the record: the vertex stage
 * scales and turns them, so a string at any size or turn is the same layout.
 * The advances and the kerning are the font's, as the UI applies them.
 */

// The next codepoint of a UTF-8 string, stepping past it; a byte that starts
// no whole sequence is one U+FFFD.
static int _utf8_next(const unsigned char** s) {
    const unsigned char* p = *s;
    int cp, extra;
    if (p[0] < 0x80) {
        *s = p + 1;
        return p[0];
    }
    if ((p[0] & 0xe0) == 0xc0)
        cp = p[0] & 0x1f, extra = 1;
    else if ((p[0] & 0xf0) == 0xe0)
        cp = p[0] & 0x0f, extra = 2;
    else if ((p[0] & 0xf8) == 0xf0)
        cp = p[0] & 0x07, extra = 3;
    else
        extra = -1, cp = 0;
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            extra = -1;
            break;
        }
        cp = cp << 6 | (p[i] & 0x3f);
    }
    if (extra < 0) {
        *s = p + 1;
        return 0xfffd;
    }
    *s = p + 1 + extra;
    return cp;
}

typedef struct TextGlyph {
    int codepoint;
    double pen; // ems from the string's start, along its baseline
} TextGlyph;

// Lays a string out: the pen of each glyph with ink, written to `out` when it
// is given, with room for one a byte, and the advance of the whole in ems.
static double _text_layout(const Font* font, const char* string, TextGlyph* out, size_t* count) {
    const double em = 1.0 / font->em_size;
    double pen = 0.0;
    int prev = 0;
    size_t n = 0;
    for (const unsigned char* s = (const unsigned char*)string; *s;) {
        const int cp = _utf8_next(&s);
        pen += font_kern_advance(font, prev, cp) * em;
        const GlyphInfo* g = cp < CANVAS_GLYPH_CODEPOINTS ? font_get_glyph(font, cp) : NULL;
        if (out && g && g->x1 > g->x0 && g->y1 > g->y0)
            out[n++] = (TextGlyph){cp, pen};
        pen += font_advance(font, cp) * em;
        prev = cp;
    }
    if (count)
        *count = n;
    return pen;
}

static bool _font_ok(const Font* font) {
    return font && font->is_sdf && font->em_size > 0.0f;
}

int64_t canvas_text_width(const CanvasText* text) {
    if (!text || !_font_ok(text->font) || !text->string)
        return 0;
    return llround(_text_layout(text->font, text->string, NULL, NULL) * (double)text->size);
}

void canvas_edit_text(CanvasEdit* edit, const CanvasText* text) {
    if (!text || !_paint_ok(edit, &text->paint, "string"))
        return;
    const Font* font = text->font;
    if (!_font_ok(font)) {
        log_error("canvas: a string with %s is refused",
                  font ? "a font that is not SDF" : "no font");
        return;
    }
    if (!_length_ok(text->size, "string", "size"))
        return;
    if (text->size == 0) {
        log_error("canvas: a string with no size is refused");
        return;
    }
    if (!text->string || !*text->string)
        return;
    const size_t len = strlen(text->string);
    if (len > CANVAS_SPLIT_MAX) {
        log_error("canvas: a string of %zu bytes is refused (at most %d)", len, CANVAS_SPLIT_MAX);
        return;
    }
    TextGlyph* glyphs = malloc(len * sizeof(TextGlyph));
    if (!glyphs) {
        log_error("canvas: out of memory laying out a string");
        return;
    }
    size_t n = 0;
    const double width = _text_layout(font, text->string, glyphs, &n);
    CanvasPiece* pieces = n ? _reserve(edit, n) : NULL;
    if (!pieces) {
        free(glyphs);
        return;
    }

    // The anchor's place on the string, in ems from its start on the baseline.
    const double em = 1.0 / font->em_size;
    const double ax = text->align_x == CANVAS_ALIGN_CENTRE  ? width * 0.5
                      : text->align_x == CANVAS_ALIGN_RIGHT ? width
                                                            : 0.0;
    const double ay = text->align_y == CANVAS_ALIGN_BOTTOM ? font->descent * em
                      : text->align_y == CANVAS_ALIGN_MIDDLE
                          ? (font->ascent + font->descent) * em * 0.5
                      : text->align_y == CANVAS_ALIGN_TOP ? font->ascent * em
                                                          : 0.0;
    const int shift = edit->tile_shift;
    const uint32_t flags = text->mirrored ? CANVAS_FLAG_MIRROR : 0u;
    // The ink of the whole string in its own frame, in ems from the anchor.
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (size_t i = 0; i < n; i++) {
        const GlyphInfo* g = font_get_glyph(font, glyphs[i].codepoint);
        CanvasPiece* piece =
            _piece(&pieces[i], shift, &text->paint, text->anchor, CANVAS_KIND_GLYPH, flags);
        piece->font = font;
        piece->record.p[0] = _rel(text->anchor.x, piece->tx, shift);
        piece->record.p[1] = _rel(text->anchor.y, piece->ty, shift);
        piece->record.p[2] = glyphs[i].codepoint;
        piece->record.f[0] = (float)(glyphs[i].pen - ax);
        piece->record.f[1] = (float)-ay;
        piece->record.f[2] = (float)text->size;
        piece->record.f[3] = (float)text->angle;
        const double x = glyphs[i].pen - ax;
        lo[0] = fmin(lo[0], x + g->x0 * em), hi[0] = fmax(hi[0], x + g->x1 * em);
        lo[1] = fmin(lo[1], -ay - g->y1 * em), hi[1] = fmax(hi[1], -ay - g->y0 * em);
    }
    // Its bound: the ink's four corners mirrored and turned about the anchor.
    // Every glyph is in the anchor's tile, so each takes the whole string's.
    const double c = cos(text->angle), s = sin(text->angle), size = (double)text->size;
    const double mirror = text->mirrored ? -1.0 : 1.0;
    double b[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (int k = 0; k < 4; k++) {
        const double x = (k & 1 ? hi[0] : lo[0]) * size * mirror,
                     y = (k & 2 ? hi[1] : lo[1]) * size;
        const double px = c * x - s * y, py = s * x + c * y;
        b[0] = fmin(b[0], px), b[1] = fmin(b[1], py);
        b[2] = fmax(b[2], px), b[3] = fmax(b[3], py);
    }
    for (size_t i = 0; i < n; i++)
        _bound(&pieces[i], text->anchor.x + (int64_t)floor(b[0]),
               text->anchor.y + (int64_t)floor(b[1]), text->anchor.x + (int64_t)ceil(b[2]),
               text->anchor.y + (int64_t)ceil(b[3]), 1);
    _commit(edit, n);
    free(glyphs);
}

void canvas_edit_remove_object(CanvasEdit* edit, uint32_t object) {
    if (!edit || object == 0)
        return;
    _step(edit, (CanvasStep){.kind = CANVAS_STEP_REMOVE_OBJECT, .object = object});
}

void canvas_edit_clear_layer(CanvasEdit* edit, CanvasLayer* layer) {
    if (!edit || !layer)
        return;
    _step(edit, (CanvasStep){.kind = CANVAS_STEP_CLEAR_LAYER, .layer = layer});
}
