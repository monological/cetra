/*
 * Ear clipping with holes, ported from Mapbox's earcut (v2.2).
 *
 * ISC License
 *
 * Copyright (c) 2016, Mapbox
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright notice
 * and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
 * IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * The algorithm is earcut's step for step: each outline a doubly linked ring,
 * each hole bridged to the outer ring at its leftmost point, then ears clipped
 * in three passes of increasing desperation -- plain, after removing collinear
 * points and curing local self-intersections, and by splitting the ring in two
 * along a valid diagonal. Past 80 points the candidate points an ear is tested
 * against are found through a z-order curve rather than the whole ring.
 *
 * What is cetra's: nodes come from one pool sized to the worst case, so a
 * triangulation allocates twice; and holes sort by x then y, so the result
 * does not depend on the order equal points arrive in.
 */

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "triangulate.h"

typedef struct EarNode {
    uint32_t i; // the point's index in the input
    double x, y;
    struct EarNode *prev, *next;
    int32_t z; // its place along the z-order curve
    struct EarNode *prev_z, *next_z;
    bool steiner; // a hole of one point
} EarNode;

typedef struct EarState {
    EarNode* pool;
    size_t used, cap;
    uint32_t* tri;
    size_t tri_count, tri_cap;
    bool failed;
    double min_x, min_y, inv_size; // inv_size 0 = no z-order
} EarState;

static EarNode* _node(EarState* s, uint32_t i, double x, double y) {
    if (s->used == s->cap) {
        s->failed = true;
        return NULL;
    }
    EarNode* p = &s->pool[s->used++];
    memset(p, 0, sizeof(*p));
    p->i = i, p->x = x, p->y = y;
    return p;
}

static void _emit(EarState* s, const EarNode* a, const EarNode* b, const EarNode* c) {
    if (s->tri_count == s->tri_cap) {
        const size_t cap = s->tri_cap ? s->tri_cap * 2 : 64;
        uint32_t* grown = realloc(s->tri, cap * 3 * sizeof(uint32_t));
        if (!grown) {
            s->failed = true;
            return;
        }
        s->tri = grown;
        s->tri_cap = cap;
    }
    uint32_t* t = s->tri + s->tri_count++ * 3;
    t[0] = a->i, t[1] = b->i, t[2] = c->i;
}

// Twice the signed area of the triangle p, q, r; negative for one that turns
// the way an ear of the outer ring does.
static double _area(const EarNode* p, const EarNode* q, const EarNode* r) {
    return (q->y - p->y) * (r->x - q->x) - (q->x - p->x) * (r->y - q->y);
}

static bool _equals(const EarNode* a, const EarNode* b) {
    return a->x == b->x && a->y == b->y;
}

static int _sign(double v) {
    return v > 0.0 ? 1 : v < 0.0 ? -1 : 0;
}

// Whether q lies on segment p r, given the three are collinear.
static bool _on_segment(const EarNode* p, const EarNode* q, const EarNode* r) {
    return q->x <= fmax(p->x, r->x) && q->x >= fmin(p->x, r->x) && q->y <= fmax(p->y, r->y) &&
           q->y >= fmin(p->y, r->y);
}

static bool _intersects(const EarNode* p1, const EarNode* q1, const EarNode* p2,
                        const EarNode* q2) {
    const int o1 = _sign(_area(p1, q1, p2)), o2 = _sign(_area(p1, q1, q2));
    const int o3 = _sign(_area(p2, q2, p1)), o4 = _sign(_area(p2, q2, q1));
    if (o1 != o2 && o3 != o4)
        return true;
    if (o1 == 0 && _on_segment(p1, p2, q1))
        return true;
    if (o2 == 0 && _on_segment(p1, q2, q1))
        return true;
    if (o3 == 0 && _on_segment(p2, p1, q2))
        return true;
    if (o4 == 0 && _on_segment(p2, q1, q2))
        return true;
    return false;
}

static bool _point_in_triangle(double ax, double ay, double bx, double by, double cx, double cy,
                               double px, double py) {
    return (cx - px) * (ay - py) >= (ax - px) * (cy - py) &&
           (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
           (bx - px) * (cy - py) >= (cx - px) * (by - py);
}

static EarNode* _insert(EarState* s, uint32_t i, double x, double y, EarNode* last) {
    EarNode* p = _node(s, i, x, y);
    if (!p)
        return last;
    if (!last) {
        p->prev = p;
        p->next = p;
    } else {
        p->next = last->next;
        p->prev = last;
        last->next->prev = p;
        last->next = p;
    }
    return p;
}

static void _remove(EarNode* p) {
    p->next->prev = p->prev;
    p->prev->next = p->next;
    if (p->prev_z)
        p->prev_z->next_z = p->next_z;
    if (p->next_z)
        p->next_z->prev_z = p->prev_z;
}

static double _signed_area(const double* xy, size_t start, size_t end) {
    double sum = 0.0;
    for (size_t i = start, j = end - 1; i < end; j = i++)
        sum += (xy[2 * j] - xy[2 * i]) * (xy[2 * i + 1] + xy[2 * j + 1]);
    return sum;
}

// A ring from points [start, end), wound one way for the outer ring and the
// other for holes.
static EarNode* _linked_list(EarState* s, const double* xy, size_t start, size_t end,
                             bool clockwise) {
    EarNode* last = NULL;
    if (clockwise == (_signed_area(xy, start, end) > 0.0)) {
        for (size_t i = start; i < end; i++)
            last = _insert(s, (uint32_t)i, xy[2 * i], xy[2 * i + 1], last);
    } else {
        for (size_t i = end; i-- > start;)
            last = _insert(s, (uint32_t)i, xy[2 * i], xy[2 * i + 1], last);
    }
    if (last && _equals(last, last->next)) {
        _remove(last);
        last = last->next;
    }
    return last;
}

// Takes out repeated and collinear points.
static EarNode* _filter_points(EarNode* start, EarNode* end) {
    if (!start)
        return start;
    if (!end)
        end = start;
    EarNode* p = start;
    bool again;
    do {
        again = false;
        if (!p->steiner && (_equals(p, p->next) || _area(p->prev, p, p->next) == 0.0)) {
            _remove(p);
            p = end = p->prev;
            if (p == p->next)
                break;
            again = true;
        } else {
            p = p->next;
        }
    } while (again || p != end);
    return end;
}

static int32_t _z_order(double x, double y, const EarState* s) {
    uint32_t ix = (uint32_t)(int32_t)((x - s->min_x) * s->inv_size);
    uint32_t iy = (uint32_t)(int32_t)((y - s->min_y) * s->inv_size);
    ix = (ix | (ix << 8)) & 0x00FF00FFu;
    ix = (ix | (ix << 4)) & 0x0F0F0F0Fu;
    ix = (ix | (ix << 2)) & 0x33333333u;
    ix = (ix | (ix << 1)) & 0x55555555u;
    iy = (iy | (iy << 8)) & 0x00FF00FFu;
    iy = (iy | (iy << 4)) & 0x0F0F0F0Fu;
    iy = (iy | (iy << 2)) & 0x33333333u;
    iy = (iy | (iy << 1)) & 0x55555555u;
    return (int32_t)(ix | (iy << 1));
}

static bool _is_ear(const EarNode* ear) {
    const EarNode *a = ear->prev, *b = ear, *c = ear->next;
    if (_area(a, b, c) >= 0.0)
        return false; // reflex
    const double x0 = fmin(a->x, fmin(b->x, c->x)), y0 = fmin(a->y, fmin(b->y, c->y));
    const double x1 = fmax(a->x, fmax(b->x, c->x)), y1 = fmax(a->y, fmax(b->y, c->y));
    for (const EarNode* p = c->next; p != a; p = p->next) {
        if (p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1 &&
            _point_in_triangle(a->x, a->y, b->x, b->y, c->x, c->y, p->x, p->y) &&
            _area(p->prev, p, p->next) >= 0.0)
            return false;
    }
    return true;
}

static bool _blocks(const EarNode* p, const EarNode* a, const EarNode* b, const EarNode* c,
                    double x0, double y0, double x1, double y1) {
    return p->x >= x0 && p->x <= x1 && p->y >= y0 && p->y <= y1 && p != a && p != c &&
           _point_in_triangle(a->x, a->y, b->x, b->y, c->x, c->y, p->x, p->y) &&
           _area(p->prev, p, p->next) >= 0.0;
}

static bool _is_ear_hashed(const EarNode* ear, const EarState* s) {
    const EarNode *a = ear->prev, *b = ear, *c = ear->next;
    if (_area(a, b, c) >= 0.0)
        return false;
    const double x0 = fmin(a->x, fmin(b->x, c->x)), y0 = fmin(a->y, fmin(b->y, c->y));
    const double x1 = fmax(a->x, fmax(b->x, c->x)), y1 = fmax(a->y, fmax(b->y, c->y));
    const int32_t min_z = _z_order(x0, y0, s), max_z = _z_order(x1, y1, s);
    const EarNode* p = ear->prev_z;
    const EarNode* n = ear->next_z;
    while (p && p->z >= min_z && n && n->z <= max_z) {
        if (_blocks(p, a, b, c, x0, y0, x1, y1))
            return false;
        p = p->prev_z;
        if (_blocks(n, a, b, c, x0, y0, x1, y1))
            return false;
        n = n->next_z;
    }
    for (; p && p->z >= min_z; p = p->prev_z)
        if (_blocks(p, a, b, c, x0, y0, x1, y1))
            return false;
    for (; n && n->z <= max_z; n = n->next_z)
        if (_blocks(n, a, b, c, x0, y0, x1, y1))
            return false;
    return true;
}

static bool _locally_inside(const EarNode* a, const EarNode* b) {
    return _area(a->prev, a, a->next) < 0.0
               ? _area(a, b, a->next) >= 0.0 && _area(a, a->prev, b) >= 0.0
               : _area(a, b, a->prev) < 0.0 || _area(a, a->next, b) < 0.0;
}

static EarNode* _cure_local_intersections(EarState* s, EarNode* start) {
    EarNode* p = start;
    do {
        const EarNode* a = p->prev;
        EarNode* b = p->next->next;
        if (!_equals(a, b) && _intersects(a, p, p->next, b) && _locally_inside(a, b) &&
            _locally_inside(b, a)) {
            _emit(s, a, p, b);
            _remove(p);
            _remove(p->next);
            p = start = b;
        }
        p = p->next;
    } while (p != start);
    return _filter_points(p, NULL);
}

static bool _intersects_polygon(const EarNode* a, const EarNode* b) {
    const EarNode* p = a;
    do {
        if (p->i != a->i && p->next->i != a->i && p->i != b->i && p->next->i != b->i &&
            _intersects(p, p->next, a, b))
            return true;
        p = p->next;
    } while (p != a);
    return false;
}

static bool _middle_inside(const EarNode* a, const EarNode* b) {
    const EarNode* p = a;
    bool inside = false;
    const double px = (a->x + b->x) / 2.0, py = (a->y + b->y) / 2.0;
    do {
        if (((p->y > py) != (p->next->y > py)) && p->next->y != p->y &&
            (px < (p->next->x - p->x) * (py - p->y) / (p->next->y - p->y) + p->x))
            inside = !inside;
        p = p->next;
    } while (p != a);
    return inside;
}

static bool _is_valid_diagonal(const EarNode* a, const EarNode* b) {
    return a->next->i != b->i && a->prev->i != b->i && !_intersects_polygon(a, b) &&
           ((_locally_inside(a, b) && _locally_inside(b, a) && _middle_inside(a, b) &&
             (_area(a->prev, a, b->prev) != 0.0 || _area(a, b->prev, b) != 0.0)) ||
            (_equals(a, b) && _area(a->prev, a, a->next) > 0.0 &&
             _area(b->prev, b, b->next) > 0.0));
}

// Links a and b by two new edges, splitting their ring in two; the second
// ring's node at b.
static EarNode* _split_polygon(EarState* s, EarNode* a, EarNode* b) {
    EarNode* a2 = _node(s, a->i, a->x, a->y);
    EarNode* b2 = _node(s, b->i, b->x, b->y);
    if (!a2 || !b2)
        return NULL;
    EarNode *an = a->next, *bp = b->prev;
    a->next = b;
    b->prev = a;
    a2->next = an;
    an->prev = a2;
    a2->prev = b2;
    b2->next = a2;
    b2->prev = bp;
    bp->next = b2;
    return b2;
}

static void _earcut_linked(EarState* s, EarNode* ear, int pass);

static void _split_earcut(EarState* s, EarNode* start) {
    EarNode* a = start;
    do {
        EarNode* b = a->next->next;
        while (b != a->prev) {
            if (a->i != b->i && _is_valid_diagonal(a, b)) {
                EarNode* c = _split_polygon(s, a, b);
                if (!c)
                    return;
                a = _filter_points(a, a->next);
                c = _filter_points(c, c->next);
                _earcut_linked(s, a, 0);
                _earcut_linked(s, c, 0);
                return;
            }
            b = b->next;
        }
        a = a->next;
    } while (a != start);
}

// Sorts a ring's z links by z, a merge sort of the list.
static EarNode* _sort_linked(EarNode* list) {
    int merges, in_size = 1;
    do {
        EarNode* p = list;
        EarNode* tail = NULL;
        list = NULL;
        merges = 0;
        while (p) {
            merges++;
            EarNode* q = p;
            int p_size = 0;
            for (int i = 0; i < in_size; i++) {
                p_size++;
                q = q->next_z;
                if (!q)
                    break;
            }
            int q_size = in_size;
            while (p_size > 0 || (q_size > 0 && q)) {
                EarNode* e;
                if (p_size != 0 && (q_size == 0 || !q || p->z <= q->z)) {
                    e = p;
                    p = p->next_z;
                    p_size--;
                } else {
                    e = q;
                    q = q->next_z;
                    q_size--;
                }
                if (tail)
                    tail->next_z = e;
                else
                    list = e;
                e->prev_z = tail;
                tail = e;
            }
            p = q;
        }
        if (tail)
            tail->next_z = NULL;
        in_size *= 2;
    } while (merges > 1);
    return list;
}

static void _index_curve(EarNode* start, const EarState* s) {
    EarNode* p = start;
    do {
        if (p->z == 0)
            p->z = _z_order(p->x, p->y, s);
        p->prev_z = p->prev;
        p->next_z = p->next;
        p = p->next;
    } while (p != start);
    p->prev_z->next_z = NULL;
    p->prev_z = NULL;
    _sort_linked(p);
}

static void _earcut_linked(EarState* s, EarNode* ear, int pass) {
    if (!ear || s->failed)
        return;
    if (!pass && s->inv_size != 0.0)
        _index_curve(ear, s);
    EarNode* stop = ear;
    while (ear->prev != ear->next) {
        const EarNode* prev = ear->prev;
        EarNode* next = ear->next;
        if (s->inv_size != 0.0 ? _is_ear_hashed(ear, s) : _is_ear(ear)) {
            _emit(s, prev, ear, next);
            _remove(ear);
            ear = next->next;
            stop = next->next;
            continue;
        }
        ear = next;
        if (ear == stop) {
            if (!pass) {
                _earcut_linked(s, _filter_points(ear, NULL), 1);
            } else if (pass == 1) {
                ear = _cure_local_intersections(s, _filter_points(ear, NULL));
                _earcut_linked(s, ear, 2);
            } else if (pass == 2) {
                _split_earcut(s, ear);
            }
            break;
        }
    }
}

static EarNode* _leftmost(EarNode* start) {
    EarNode *p = start, *left = start;
    do {
        if (p->x < left->x || (p->x == left->x && p->y < left->y))
            left = p;
        p = p->next;
    } while (p != start);
    return left;
}

static bool _sector_contains_sector(const EarNode* m, const EarNode* p) {
    return _area(m->prev, m, p->prev) < 0.0 && _area(p->next, m, m->next) < 0.0;
}

// The outer ring's point a hole is bridged to: where a ray left from the
// hole's leftmost point first meets the ring, or the point nearest that ray's
// direction that can see the hole.
static EarNode* _find_hole_bridge(const EarNode* hole, EarNode* outer) {
    EarNode* p = outer;
    const double hx = hole->x, hy = hole->y;
    double qx = -INFINITY;
    EarNode* m = NULL;
    do {
        if (hy <= p->y && hy >= p->next->y && p->next->y != p->y) {
            const double x = p->x + (hy - p->y) * (p->next->x - p->x) / (p->next->y - p->y);
            if (x <= hx && x > qx) {
                qx = x;
                m = p->x < p->next->x ? p : p->next;
                if (x == hx)
                    return m; // the hole touches the ring here
            }
        }
        p = p->next;
    } while (p != outer);
    if (!m)
        return NULL;
    const EarNode* stop = m;
    const double mx = m->x, my = m->y;
    double tan_min = INFINITY;
    p = m;
    do {
        if (hx >= p->x && p->x >= mx && hx != p->x &&
            _point_in_triangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, p->x, p->y)) {
            const double tan_ = fabs(hy - p->y) / (hx - p->x);
            if (_locally_inside(p, hole) &&
                (tan_ < tan_min ||
                 (tan_ == tan_min &&
                  (p->x > m->x || (p->x == m->x && _sector_contains_sector(m, p)))))) {
                m = p;
                tan_min = tan_;
            }
        }
        p = p->next;
    } while (p != stop);
    return m;
}

static int _compare_leftmost(const void* a, const void* b) {
    const EarNode* p = *(EarNode* const*)a;
    const EarNode* q = *(EarNode* const*)b;
    if (p->x != q->x)
        return p->x < q->x ? -1 : 1;
    if (p->y != q->y)
        return p->y < q->y ? -1 : 1;
    return p->i < q->i ? -1 : p->i > q->i ? 1 : 0;
}

static EarNode* _eliminate_holes(EarState* s, const double* xy, size_t count, const size_t* holes,
                                 size_t hole_count, EarNode* outer) {
    EarNode** queue = malloc(hole_count * sizeof(EarNode*));
    if (!queue) {
        s->failed = true;
        return outer;
    }
    size_t n = 0;
    for (size_t h = 0; h < hole_count; h++) {
        const size_t start = holes[h];
        const size_t end = h + 1 < hole_count ? holes[h + 1] : count;
        EarNode* list = _linked_list(s, xy, start, end, false);
        if (!list)
            continue;
        if (list == list->next)
            list->steiner = true;
        queue[n++] = _leftmost(list);
    }
    qsort(queue, n, sizeof(EarNode*), _compare_leftmost);
    for (size_t h = 0; h < n && !s->failed; h++) {
        EarNode* bridge = _find_hole_bridge(queue[h], outer);
        if (!bridge)
            continue;
        EarNode* reverse = _split_polygon(s, bridge, queue[h]);
        if (!reverse)
            break;
        _filter_points(reverse, reverse->next);
        outer = _filter_points(bridge, bridge->next);
    }
    free(queue);
    return outer;
}

bool triangulate_polygon(const double* xy, size_t count, const size_t* holes, size_t hole_count,
                         Triangulation* out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    const size_t outer_end = hole_count ? holes[0] : count;
    if (!xy || count < 3 || outer_end < 3 || count > UINT32_MAX)
        return false;
    for (size_t h = 0; h < hole_count; h++) {
        const size_t end = h + 1 < hole_count ? holes[h + 1] : count;
        if (holes[h] < outer_end || end < holes[h] + 3 || end > count)
            return false;
    }

    // Every point once, two more for each hole's bridge and each diagonal
    // split; a ring of n points splits at most n times.
    EarState s = {.cap = count * 3 + hole_count * 2 + 16};
    s.pool = malloc(s.cap * sizeof(EarNode));
    if (!s.pool)
        return false;

    EarNode* outer = _linked_list(&s, xy, 0, outer_end, true);
    if (outer && outer->next != outer->prev) {
        if (hole_count)
            outer = _eliminate_holes(&s, xy, count, holes, hole_count, outer);
        if (count > 80) {
            double x1 = xy[0], y1 = xy[1];
            s.min_x = x1, s.min_y = y1;
            for (size_t i = 1; i < outer_end; i++) {
                s.min_x = fmin(s.min_x, xy[2 * i]), s.min_y = fmin(s.min_y, xy[2 * i + 1]);
                x1 = fmax(x1, xy[2 * i]), y1 = fmax(y1, xy[2 * i + 1]);
            }
            const double size = fmax(x1 - s.min_x, y1 - s.min_y);
            s.inv_size = size != 0.0 ? 32767.0 / size : 0.0;
        }
        _earcut_linked(&s, outer, 0);
    }
    free(s.pool);
    if (s.failed) {
        log_error("triangulate: out of memory");
        free(s.tri);
        return false;
    }

    out->indices = s.tri;
    out->count = s.tri_count;
    return true;
}

void free_triangulation(Triangulation* triangulation) {
    if (!triangulation)
        return;
    free(triangulation->indices);
    memset(triangulation, 0, sizeof(*triangulation));
}
