#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas_record.h"
#include "ext/log.h"

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
