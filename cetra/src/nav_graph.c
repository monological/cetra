#include "nav_graph.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"

// A corner sharper than this is not rounded: the agent stops at the place and turns there.
#define NAV_ROUND_LIMIT (150.0f * GLM_PIf / 180.0f)
// A corner is rounded within this share of either link's length at most, so two corners on
// one short link never meet.
#define NAV_ROUND_SHARE 0.45f

NavGraph* create_nav_graph(void) {
    NavGraph* g = calloc(1, sizeof(NavGraph));
    if (!g)
        log_error("create_nav_graph: out of memory");
    return g;
}

void free_nav_graph(NavGraph* graph) {
    if (!graph)
        return;
    free(graph->nodes);
    free(graph->links);
    free(graph);
}

void nav_graph_set_kind(NavGraph* graph, int kind, const NavKind* desc) {
    if (!graph || !desc || kind < 0 || kind >= NAV_KIND_MAX) {
        log_error("nav_graph_set_kind: no kind %d", kind);
        return;
    }
    graph->kinds[kind] = *desc;
}

static float kind_scale(const NavGraph* g, const NavQuery* q, int kind) {
    float s = q && q->cost_scale[kind] > 0.0f ? q->cost_scale[kind] : g->kinds[kind].cost_scale;
    return s > 0.0f ? s : 1.0f;
}

static bool kind_allowed(const NavQuery* q, int kind) {
    return !q || !q->kinds || (q->kinds & (1u << kind));
}

int nav_graph_add_node(NavGraph* graph, const char* name, const vec3 position, float radius,
                       uint32_t tags) {
    if (!graph || !name || nav_graph_find(graph, name) >= 0) {
        log_error("nav_graph_add_node: '%s' refused: no name, or one in use", name ? name : "");
        return -1;
    }
    if (graph->node_count == graph->node_cap) {
        const int cap = graph->node_cap ? 2 * graph->node_cap : 32;
        NavNode* grown = realloc(graph->nodes, (size_t)cap * sizeof(NavNode));
        if (!grown) {
            log_error("nav_graph_add_node: out of memory");
            return -1;
        }
        graph->nodes = grown;
        graph->node_cap = cap;
    }
    NavNode* n = &graph->nodes[graph->node_count];
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, NAV_NAME_MAX - 1);
    glm_vec3_copy((float*)position, n->position);
    n->radius = radius;
    n->tags = tags;
    return graph->node_count++;
}

int nav_graph_find(const NavGraph* graph, const char* name) {
    if (!graph || !name)
        return -1;
    for (int i = 0; i < graph->node_count; i++)
        if (!strncmp(graph->nodes[i].name, name, NAV_NAME_MAX))
            return i;
    return -1;
}

static float horizontal(const vec3 a, const vec3 b) {
    const float dx = b[0] - a[0], dz = b[2] - a[2];
    return sqrtf(dx * dx + dz * dz);
}

int nav_graph_link(NavGraph* graph, int from, int to, int kind, const NavShapeDesc* shape) {
    if (!graph || from < 0 || to < 0 || from >= graph->node_count || to >= graph->node_count ||
        from == to || kind < 0 || kind >= NAV_KIND_MAX || !shape) {
        log_error("nav_graph_link: %d -> %d of kind %d refused", from, to, kind);
        return -1;
    }
    if (graph->link_count == graph->link_cap) {
        const int cap = graph->link_cap ? 2 * graph->link_cap : 64;
        NavLink* grown = realloc(graph->links, (size_t)cap * sizeof(NavLink));
        if (!grown) {
            log_error("nav_graph_link: out of memory");
            return -1;
        }
        graph->links = grown;
        graph->link_cap = cap;
    }
    NavLink* l = &graph->links[graph->link_count];
    memset(l, 0, sizeof(*l));
    l->from = from;
    l->to = to;
    l->kind = kind;
    l->shape = *shape;
    l->enabled = true;
    const float* a = graph->nodes[from].position;
    const float* b = graph->nodes[to].position;
    // A walk or a flight is advanced by how far the agent went over the ground, and a line
    // or an arc costs its chord.
    l->length = shape->shape == NAV_SHAPE_STEPS ? horizontal(a, b)
                                                : glm_vec3_distance((float*)a, (float*)b);
    if (l->length < 1e-4f) {
        log_error("nav_graph_link: %s -> %s has no length", graph->nodes[from].name,
                  graph->nodes[to].name);
        return -1;
    }
    l->cost = l->length * kind_scale(graph, NULL, kind);
    return graph->link_count++;
}

int nav_graph_link_both(NavGraph* graph, int a, int b, int kind, const NavShapeDesc* shape) {
    const int first = nav_graph_link(graph, a, b, kind, shape);
    if (first < 0)
        return -1;
    NavShapeDesc back = *shape;
    if (shape->shape == NAV_SHAPE_STEPS && shape->rise > 0.0f && shape->going > 0.0f) {
        // The flight walked from the other end: its first riser is as far from that end as
        // the last one was from it.
        const float dy = fabsf(graph->nodes[b].position[1] - graph->nodes[a].position[1]);
        const int risers = (int)lroundf(dy / shape->rise);
        const float flight = shape->nosing + (float)(risers > 0 ? risers - 1 : 0) * shape->going;
        back.nosing = graph->links[first].length - flight;
    }
    if (nav_graph_link(graph, b, a, kind, &back) < 0)
        return -1;
    return first;
}

// The height of a flight's profile a horizontal distance x into it.
static float steps_height(const NavShapeDesc* s, float y0, float y1, float x) {
    const float dy = y1 - y0;
    const int risers = s->rise > 0.0f ? (int)lroundf(fabsf(dy) / s->rise) : 0;
    if (risers <= 0 || s->going <= 0.0f)
        return y0;
    const float each = dy / (float)risers;
    // Treads: a riser counted as soon as it is passed.
    float k = floorf((x - s->nosing) / s->going) + 1.0f;
    const float tread = y0 + each * glm_clamp(k, 0.0f, (float)risers);
    // The line through the nosings, which passes each riser half way up it.
    const float pitch =
        y0 + each * glm_clamp((x - s->nosing) / s->going + 0.5f, 0.0f, (float)risers);
    return glm_lerp(tread, pitch, glm_clamp(s->hop, 0.0f, 1.0f));
}

// An arc's height a fraction u across: a parabola in u from y0 to y1 whose top is exactly
// `apex` above the higher end. With u in proportion to time, as a follower driven by a jump's
// clock has it, that is a body in free flight.
static float arc_height(float apex, float y0, float y1, float u) {
    const float d = y1 - y0;
    const float top = fmaxf(y0, y1) + fmaxf(apex, 0.0f);
    const float m = top - y0 - 0.5f * d;
    const float k = 2.0f * m + sqrtf(fmaxf(4.0f * m * m - d * d, 0.0f));
    return y0 + d * u + k * u * (1.0f - u);
}

void nav_link_point(const NavGraph* graph, int link, float u, vec3 out) {
    const NavLink* l = &graph->links[link];
    const float* a = graph->nodes[l->from].position;
    const float* b = graph->nodes[l->to].position;
    u = glm_clamp(u, 0.0f, 1.0f);
    glm_vec3_lerp((float*)a, (float*)b, u, out);
    if (l->shape.shape == NAV_SHAPE_STEPS)
        out[1] = steps_height(&l->shape, a[1], b[1], u * l->length);
    else if (l->shape.shape == NAV_SHAPE_ARC)
        out[1] = arc_height(l->shape.apex, a[1], b[1], u);
}

static float link_heading(const NavGraph* g, int link) {
    const NavLink* l = &g->links[link];
    const float* a = g->nodes[l->from].position;
    const float* b = g->nodes[l->to].position;
    return atan2f(b[0] - a[0], b[2] - a[2]);
}

static float wrap_angle(float a) {
    while (a > GLM_PIf)
        a -= 2.0f * GLM_PIf;
    while (a < -GLM_PIf)
        a += 2.0f * GLM_PIf;
    return a;
}

bool nav_graph_route(const NavGraph* graph, int from, int to, const NavQuery* query,
                     NavRoute* out) {
    if (!graph || !out || from < 0 || to < 0 || from >= graph->node_count ||
        to >= graph->node_count)
        return false;
    out->count = 0;
    out->cost = 0.0f;
    if (from == to)
        return true;
    const int n = graph->node_count;
    float* g = malloc((size_t)n * sizeof(float));
    float* f = malloc((size_t)n * sizeof(float));
    int* via = malloc((size_t)n * sizeof(int));
    unsigned char* state = calloc((size_t)n, 1); // 0 unseen, 1 open, 2 closed
    if (!g || !f || !via || !state) {
        free(g);
        free(f);
        free(via);
        free(state);
        log_error("nav_graph_route: out of memory");
        return false;
    }
    // The cheapest a metre can be under this query, which keeps the straight-line estimate
    // from ever overstating what is left.
    float cheapest = FLT_MAX;
    for (int k = 0; k < NAV_KIND_MAX; k++)
        if (kind_allowed(query, k))
            cheapest = fminf(cheapest, kind_scale(graph, query, k));
    if (cheapest == FLT_MAX)
        cheapest = 1.0f;
    for (int i = 0; i < n; i++) {
        g[i] = FLT_MAX;
        via[i] = -1;
    }
    const float* goal = graph->nodes[to].position;
    g[from] = 0.0f;
    f[from] = glm_vec3_distance(graph->nodes[from].position, (float*)goal) * cheapest;
    state[from] = 1;
    bool found = false;
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (state[i] == 1 && (best < 0 || f[i] < f[best]))
                best = i; // the lowest index wins a tie, since it is met first
        if (best < 0)
            break;
        if (best == to) {
            found = true;
            break;
        }
        state[best] = 2;
        for (int li = 0; li < graph->link_count; li++) {
            const NavLink* l = &graph->links[li];
            if (l->from != best || !l->enabled || !kind_allowed(query, l->kind) ||
                state[l->to] == 2)
                continue;
            const float cost = g[best] + l->length * kind_scale(graph, query, l->kind);
            if (cost < g[l->to]) {
                g[l->to] = cost;
                f[l->to] =
                    cost + glm_vec3_distance(graph->nodes[l->to].position, (float*)goal) * cheapest;
                via[l->to] = li;
                state[l->to] = 1;
            }
        }
    }
    bool ok = false;
    if (found) {
        int count = 0;
        for (int at = to; at != from; at = graph->links[via[at]].from)
            count++;
        if (count <= NAV_ROUTE_MAX) {
            out->count = count;
            out->cost = g[to];
            for (int at = to, k = count - 1; at != from; at = graph->links[via[at]].from, k--)
                out->links[k] = via[at];
            ok = true;
        }
    }
    free(g);
    free(f);
    free(via);
    free(state);
    return ok;
}

bool nav_follower_start(NavFollower* f, const NavGraph* graph, int from, int to,
                        const NavQuery* query) {
    memset(f, 0, sizeof(*f));
    f->graph = graph;
    if (query)
        f->query = *query;
    f->at = from;
    f->goal = to;
    if (!nav_graph_route(graph, from, to, query, &f->route)) {
        f->arrived = true;
        f->goal = from;
        return false;
    }
    f->arrived = f->route.count == 0;
    return true;
}

static void finish_link(NavFollower* f) {
    f->at = f->graph->links[f->route.links[f->leg]].to;
    f->leg++;
    f->along = 0.0f;
    if (f->leg >= f->route.count)
        f->arrived = true;
}

float nav_follower_advance(NavFollower* f, float metres) {
    float used = 0.0f;
    while (metres > 0.0f && !f->arrived) {
        const NavLink* l = &f->graph->links[f->route.links[f->leg]];
        if (f->graph->kinds[l->kind].drive == NAV_DRIVE_PROGRESS)
            break;
        const float left = l->length - f->along;
        if (metres < left) {
            f->along += metres;
            used += metres;
            break;
        }
        metres -= left;
        used += left;
        finish_link(f);
    }
    return used;
}

void nav_follower_set_progress(NavFollower* f, float progress) {
    if (f->arrived)
        return;
    const NavLink* l = &f->graph->links[f->route.links[f->leg]];
    if (f->graph->kinds[l->kind].drive != NAV_DRIVE_PROGRESS)
        return;
    if (progress >= 1.0f)
        finish_link(f);
    else
        f->along = fmaxf(progress, 0.0f);
}

// Whether the corner from link `a` into link `b` at their shared place is rounded, and how
// far either side of the place: both of smoothed kinds driven by distance, and not too sharp.
static float corner_reach(const NavGraph* g, int a, int b) {
    const NavLink* la = &g->links[a];
    const NavLink* lb = &g->links[b];
    const NavKind* ka = &g->kinds[la->kind];
    const NavKind* kb = &g->kinds[lb->kind];
    if (!ka->smooth || !kb->smooth || ka->drive != NAV_DRIVE_DISTANCE ||
        kb->drive != NAV_DRIVE_DISTANCE)
        return 0.0f;
    if (fabsf(wrap_angle(link_heading(g, b) - link_heading(g, a))) > NAV_ROUND_LIMIT)
        return 0.0f;
    const float r = g->nodes[la->to].radius;
    return fminf(r, NAV_ROUND_SHARE * fminf(la->length, lb->length));
}

// A quadratic Bezier through the corner: from `r` before the place on the way in, past the
// place, to `r` after it on the way out. Half way through it the follower crosses from one
// link to the next, so the two halves are drawn from either side and meet there.
static void corner(const NavGraph* g, int in, int out_link, float r, float t, vec3 pos,
                   vec3 tangent) {
    vec3 p0 = {0.0f, 0.0f, 0.0f}, p2 = {0.0f, 0.0f, 0.0f};
    nav_link_point(g, in, 1.0f - r / g->links[in].length, p0);
    nav_link_point(g, out_link, r / g->links[out_link].length, p2);
    const float* p1 = g->nodes[g->links[in].to].position;
    const float s = 1.0f - t;
    for (int i = 0; i < 3; i++) {
        pos[i] = s * s * p0[i] + 2.0f * s * t * p1[i] + t * t * p2[i];
        tangent[i] = 2.0f * s * (p1[i] - p0[i]) + 2.0f * t * (p2[i] - p1[i]);
    }
}

void nav_follower_sample(const NavFollower* f, NavSample* out) {
    memset(out, 0, sizeof(*out));
    const NavGraph* g = f->graph;
    out->link = -1;
    out->arrived = f->arrived;
    if (f->arrived || f->route.count == 0) {
        glm_vec3_copy(g->nodes[f->at].position, out->position);
        if (f->route.count > 0) {
            const int last = f->route.links[f->route.count - 1];
            out->heading = link_heading(g, last);
        }
        out->tangent[0] = sinf(out->heading);
        out->tangent[2] = cosf(out->heading);
        return;
    }
    const int li = f->route.links[f->leg];
    const NavLink* l = &g->links[li];
    const bool by_distance = g->kinds[l->kind].drive == NAV_DRIVE_DISTANCE;
    const float u = by_distance ? f->along / l->length : f->along;
    out->link = li;
    out->kind = l->kind;
    out->progress = u;
    nav_link_point(g, li, u, out->position);
    vec3 behind = {0.0f, 0.0f, 0.0f}, ahead = {0.0f, 0.0f, 0.0f};
    nav_link_point(g, li, fmaxf(u - 0.01f, 0.0f), behind);
    nav_link_point(g, li, fminf(u + 0.01f, 1.0f), ahead);
    glm_vec3_sub(ahead, behind, out->tangent);

    const int next = f->leg + 1 < f->route.count ? f->route.links[f->leg + 1] : -1;
    const int prev = f->leg > 0 ? f->route.links[f->leg - 1] : -1;
    if (next >= 0)
        out->turn = wrap_angle(link_heading(g, next) - link_heading(g, li));
    if (by_distance) {
        const float s = f->along;
        const float r_end = next >= 0 ? corner_reach(g, li, next) : 0.0f;
        const float r_start = prev >= 0 ? corner_reach(g, prev, li) : 0.0f;
        if (r_end > 0.0f && s > l->length - r_end)
            corner(g, li, next, r_end, 0.5f * (s - (l->length - r_end)) / r_end, out->position,
                   out->tangent);
        else if (r_start > 0.0f && s < r_start)
            corner(g, prev, li, r_start, 0.5f + 0.5f * s / r_start, out->position, out->tangent);
    }
    glm_vec3_normalize(out->tangent);
    out->heading = atan2f(out->tangent[0], out->tangent[2]);
    // Up or down a profile the level part may vanish; the link's own direction stands in.
    if (fabsf(out->tangent[0]) + fabsf(out->tangent[2]) < 1e-3f)
        out->heading = link_heading(g, li);
}

bool nav_follower_replan(NavFollower* f, int to) {
    if (f->arrived) {
        NavFollower fresh;
        if (!nav_follower_start(&fresh, f->graph, f->at, to, &f->query))
            return false;
        *f = fresh;
        return true;
    }
    const int current = f->route.links[f->leg];
    NavRoute rest;
    if (!nav_graph_route(f->graph, f->graph->links[current].to, to, &f->query, &rest) ||
        rest.count + 1 > NAV_ROUTE_MAX)
        return false;
    f->route.links[0] = current;
    for (int i = 0; i < rest.count; i++)
        f->route.links[i + 1] = rest.links[i];
    f->route.count = rest.count + 1;
    f->route.cost = rest.cost;
    f->leg = 0;
    f->goal = to;
    return true;
}

int nav_graph_check(const NavGraph* graph, NavProbeFn probe, void* user, float lift, float step) {
    if (!graph || !probe || step <= 0.0f)
        return 0;
    int blocked = 0;
    for (int li = 0; li < graph->link_count; li++) {
        const NavLink* l = &graph->links[li];
        if (!l->enabled)
            continue;
        // Steps and arcs are walked finer than their chord says: their profile is longer.
        const float span =
            l->length + fabsf(graph->nodes[l->to].position[1] - graph->nodes[l->from].position[1]) +
            (l->shape.shape == NAV_SHAPE_ARC ? 2.0f * l->shape.apex : 0.0f);
        const int steps = (int)ceilf(span / step) + 1;
        vec3 a = {0.0f, 0.0f, 0.0f}, b = {0.0f, 0.0f, 0.0f};
        nav_link_point(graph, li, 0.0f, a);
        a[1] += lift;
        for (int k = 1; k <= steps; k++) {
            nav_link_point(graph, li, (float)k / (float)steps, b);
            b[1] += lift;
            if (probe(a, b, user)) {
                log_warn("nav: %s -> %s (%s) is blocked near (%.2f, %.2f, %.2f)",
                         graph->nodes[l->from].name, graph->nodes[l->to].name,
                         graph->kinds[l->kind].name ? graph->kinds[l->kind].name : "?",
                         (double)a[0], (double)a[1], (double)a[2]);
                blocked++;
                break;
            }
            glm_vec3_copy(b, a);
        }
    }
    return blocked;
}
