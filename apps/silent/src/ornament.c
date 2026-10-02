#include <math.h>

#include "layout.h"
#include "mats.h"
#include "ornament.h"

#define BASE_OUT     0.05f // how far the base stands proud of the boards
#define BATTEN_PITCH 0.42f
#define BATTEN_W     0.05f
#define BATTEN_T     0.025f
#define HOOD_W       0.06f // the hood moulding over the casing
#define HOOD_T       0.07f
#define MULLION      0.05f // tracery's bars, either side of the glass
#define TRACERY_T    0.03f
// A lancet at least this wide is two lights under a ring.
#define TWO_LIGHTS 0.65f

// The side of a wall drawn in `wf` that lies `side` along its inner axis: +1 its inner side,
// -1 its outer.
static Facade side_of(const KitWallFrame* wf, float thick, int side) {
    const float out = (float)(side * wf->inner);
    return (Facade){wf->f, wf->at + out * 0.5f * thick, wf->at, out};
}

Facade facade_of(const KitWall* w) {
    const KitWallFrame wf = kit_wall_frame(w);
    return side_of(&wf, w->thick, -1);
}

Facade facade_inner(const KitWall* w) {
    const KitWallFrame wf = kit_wall_frame(w);
    return side_of(&wf, w->thick, 1);
}

Facade facade_of_frame(const KitFrame* f, const KitWall* w) {
    const KitWallFrame wf = {*f, w->at, w->inner >= 0 ? 1 : -1};
    return side_of(&wf, w->thick, -1);
}

void ornament_casing(Kit* kit, const Facade* s, int mat, const KitOpening* o) {
    kit_frame_surround(kit, &s->f, mat, o, ORNAMENT_CASING_W, false, s->face,
                       s->face + s->out * ORNAMENT_CASING_T);
}

static bool doorway(const KitOpening* o) {
    return o->bottom <= FLOOR_Y + 0.01f;
}

// The water table: a dressed stone sloping off the base's top, out from the face.
static void water_table(Kit* kit, const Facade* s, float a0, float a1) {
    const float f = s->face, o = s->out;
    const vec2 profile[4] = {{f, BASE_TOP},
                             {f + o * (BASE_OUT + 0.03f), BASE_TOP},
                             {f + o * (BASE_OUT + 0.03f), BASE_TOP + 0.025f},
                             {f, BASE_TOP + 0.09f}};
    kit_frame_run(kit, &s->f, MAT_STONE, profile, 4, a0, a1);
}

void ornament_base(Kit* kit, const Facade* s, float a0, float a1, const KitOpening* openings,
                   int count) {
    // Along the wall in pieces, stopping at each doorway's sill.
    float cursor = a0;
    for (int pass = 0; pass <= count; pass++) {
        // The next doorway along, if any is left.
        const KitOpening* next = NULL;
        for (int i = 0; i < count; i++)
            if (doorway(&openings[i]) && openings[i].from >= cursor - 1e-4f &&
                (!next || openings[i].from < next->from))
                next = &openings[i];
        const float end = next ? next->from : a1;
        if (end - cursor > 1e-3f) {
            kit_frame_box(kit, &s->f, MAT_FOUNDATION, cursor, end, 0.0f, BASE_TOP, s->face,
                          s->face + s->out * BASE_OUT, false);
            water_table(kit, s, cursor, end);
        }
        if (!next)
            break;
        kit_frame_box(kit, &s->f, MAT_FOUNDATION, next->from, next->to, 0.0f, next->bottom, s->face,
                      s->face + s->out * BASE_OUT, false);
        cursor = next->to;
    }
}

// Where the batten at `a` is interrupted by openings, as y ranges: each opening whose casing
// it would cross, from under its sill to over its hood. Returns the count, sorted.
static int blocked(float a, const KitOpening* openings, int count, float lo[], float hi[]) {
    int n = 0;
    const float margin = ORNAMENT_CASING_W + HOOD_W + 0.02f;
    for (int i = 0; i < count; i++) {
        const KitOpening* o = &openings[i];
        if (a + 0.5f * BATTEN_W < o->from - margin || a - 0.5f * BATTEN_W > o->to + margin)
            continue;
        const KitOpening grown = kit_opening_grow(o, margin);
        lo[n] = doorway(o) ? -1.0f : o->bottom - 0.12f;
        hi[n] = grown.top + grown.rise + 0.02f;
        for (int j = n; j > 0 && lo[j] < lo[j - 1]; j--) {
            const float tl = lo[j], th = hi[j];
            lo[j] = lo[j - 1];
            hi[j] = hi[j - 1];
            lo[j - 1] = tl;
            hi[j - 1] = th;
        }
        n++;
    }
    return n;
}

static void batten(Kit* kit, const Facade* s, float a, float y0, float y1) {
    if (y1 - y0 < 0.08f)
        return;
    kit_frame_box(kit, &s->f, MAT_SIDING_DARK, a - 0.5f * BATTEN_W, a + 0.5f * BATTEN_W, y0, y1,
                  s->face, s->face + s->out * BATTEN_T, false);
}

void ornament_battens(Kit* kit, const Facade* s, float a0, float a1, float y0, float y1,
                      const KitOpening* openings, int count) {
    const int n = (int)floorf((a1 - a0) / BATTEN_PITCH);
    const float start = a0 + 0.5f * ((a1 - a0) - (float)(n - 1) * BATTEN_PITCH);
    for (int i = 0; i < n; i++) {
        const float a = start + (float)i * BATTEN_PITCH;
        float lo[KIT_MAX_OPENINGS], hi[KIT_MAX_OPENINGS];
        const int nb = blocked(a, openings, count, lo, hi);
        float y = y0;
        for (int b = 0; b < nb; b++) {
            batten(kit, s, a, y, fminf(lo[b], y1));
            y = fmaxf(y, hi[b]);
        }
        batten(kit, s, a, y, y1);
    }
}

void ornament_gable_battens(Kit* kit, const Facade* s, float a0, float a1, float y0, float apex_a,
                            float apex_y, float pitch) {
    const int n = (int)floorf((a1 - a0) / BATTEN_PITCH);
    const float start = a0 + 0.5f * ((a1 - a0) - (float)(n - 1) * BATTEN_PITCH);
    for (int i = 0; i < n; i++) {
        const float a = start + (float)i * BATTEN_PITCH;
        // Its top under the roof's line at its far edge, so no corner pokes through.
        const float top = apex_y - pitch * (fabsf(a - apex_a) + 0.5f * BATTEN_W) - 0.05f;
        batten(kit, s, a, y0, top);
    }
}

// A ring of stone or wood, centred at c in (a, y), from radius r_in to r_out: two halves, each
// out round its outer arc and back round its inner one.
static void ring(Kit* kit, const KitFrame* f, int mat, const vec2 c, float r_out, float r_in,
                 float d0, float d1) {
    enum { SEG = 10 };
    for (int side = 0; side < 2; side++) {
        vec2 pts[2 * (SEG + 1)];
        int n = 0;
        for (int i = 0; i <= SEG; i++) {
            const float t = 0.5f * GLM_PIf + (side ? -1.0f : 1.0f) * GLM_PIf * (float)i / SEG;
            glm_vec2_copy((vec2){c[0] + r_out * cosf(t), c[1] + r_out * sinf(t)}, pts[n++]);
        }
        for (int i = SEG; i >= 0; i--) {
            const float t = 0.5f * GLM_PIf + (side ? -1.0f : 1.0f) * GLM_PIf * (float)i / SEG;
            glm_vec2_copy((vec2){c[0] + r_in * cosf(t), c[1] + r_in * sinf(t)}, pts[n++]);
        }
        kit_frame_extrude(kit, f, mat, pts, n, d0, d1);
    }
}

/*
 * Two lights under a ring, the commonest Gothic window: a mullion up the middle to the
 * springing line, each light's own pointed head as a bar, and a ring in the space left
 * between them and the window's head. All in the plane of the glass, straddling it.
 */
static void two_lights(Kit* kit, const Facade* s, const KitOpening* o) {
    const float mid = 0.5f * (o->from + o->to), w = o->to - o->from;
    const float d0 = s->mid - TRACERY_T, d1 = s->mid + TRACERY_T;
    const float light = 0.5f * (w - MULLION);
    kit_frame_box(kit, &s->f, MAT_TRIM, mid - 0.5f * MULLION, mid + 0.5f * MULLION, o->bottom,
                  o->top + 0.1f * o->rise, d0, d1, false);
    const float sub_rise = 0.85f * light;
    const KitOpening left = {o->from, mid - 0.5f * MULLION, o->bottom,
                             o->top,  KIT_ARCH_POINTED,     sub_rise};
    const KitOpening right = {mid + 0.5f * MULLION, o->to,   o->bottom, o->top,
                              KIT_ARCH_POINTED,     sub_rise};
    kit_frame_surround(kit, &s->f, MAT_TRIM, &left, 0.035f, true, d0, d1);
    kit_frame_surround(kit, &s->f, MAT_TRIM, &right, 0.035f, true, d0, d1);
    // The ring sits midway between the lights' crowns and the window's.
    const float low = o->top + sub_rise + 0.035f, high = o->top + o->rise;
    const float r = fminf(0.42f * (high - low), 0.3f * w);
    if (r > 0.04f)
        ring(kit, &s->f, MAT_TRIM, (vec2){mid, 0.5f * (low + high)}, r, r - 0.03f, d0, d1);
}

void ornament_window(Kit* kit, const Facade* s, const KitOpening* o) {
    const float f = s->face, out = s->out;
    ornament_casing(kit, s, MAT_TRIM, o);
    // The sill, deeper than the casing and past it either side, tipped under the glass, and
    // standing a centimetre over the opening's foot so the wall's own sill face is under it, not
    // in the same plane. A doorway has its threshold instead.
    if (!doorway(o))
        kit_frame_box(kit, &s->f, MAT_TRIM, o->from - ORNAMENT_CASING_W - 0.05f,
                      o->to + ORNAMENT_CASING_W + 0.05f, o->bottom - 0.07f, o->bottom + 0.01f,
                      s->mid, f + out * 0.09f, false);
    // The hood over the casing, and its two stops dropping past the springing line.
    const KitOpening casing = kit_opening_grow(o, ORNAMENT_CASING_W);
    kit_frame_surround(kit, &s->f, MAT_TRIM, &casing, HOOD_W, true, f, f + out * HOOD_T);
    const float stop = casing.top, over = kit_opening_arched(o) ? 0.0f : HOOD_W;
    for (int side = -1; side <= 1; side += 2) {
        const float edge = side < 0 ? casing.from : casing.to;
        kit_frame_box(kit, &s->f, MAT_TRIM, edge, edge + (float)side * (HOOD_W + 0.05f),
                      stop - 0.16f, stop + over, f, f + out * HOOD_T, false);
    }
    // Tracery is a window's: a doorway is left clear to walk through.
    if (o->arch == KIT_ARCH_POINTED && o->to - o->from >= TWO_LIGHTS && !doorway(o))
        two_lights(kit, s, o);
}

/*
 * The cusped foot: a row of little arches hanging from the board, each a curve lifting from
 * a cusp at CUSP_DROP below the roof's edge to ARCH_DROP at its middle. Sized to a whole
 * number of arches along the rake.
 */
#define CUSP_DROP  0.38f
#define ARCH_DROP  0.18f
#define ARCH_SPAN  0.42f
#define ARCH_STEPS 5

void ornament_bargeboard(Kit* kit, const KitFrame* f, const vec2 eave, const vec2 apex, float d0,
                         float d1) {
    vec2 e = {apex[0] - eave[0], apex[1] - eave[1]};
    const float len = glm_vec2_norm(e);
    if (len < 0.5f)
        return;
    glm_vec2_scale(e, 1.0f / len, e);
    // Down off the roof's line, square to it.
    vec2 down = {e[1], -e[0]};
    if (down[1] > 0.0f)
        glm_vec2_negate(down);
    int arches = (int)roundf(len / ARCH_SPAN);
    if (arches * ARCH_STEPS + 3 > KIT_MAX_OUTLINE)
        arches = (KIT_MAX_OUTLINE - 3) / ARCH_STEPS;
    vec2 pts[KIT_MAX_OUTLINE];
    int n = 0;
    glm_vec2_copy((float*)eave, pts[n++]);
    glm_vec2_copy((float*)apex, pts[n++]);
    // Back along the foot from the apex, a cusp at every arch's ends.
    for (int j = arches - 1; j >= 0; j--)
        for (int k = ARCH_STEPS; k > 0; k--) {
            const float u = (float)k / ARCH_STEPS;
            const float s = len * ((float)j + u) / (float)arches;
            // sinf(pi) is a hair under zero in float, and powf of it is NaN.
            const float lift = powf(fmaxf(sinf(GLM_PIf * u), 0.0f), 0.6f);
            const float drop = CUSP_DROP - (CUSP_DROP - ARCH_DROP) * lift;
            glm_vec2_copy(
                (vec2){eave[0] + e[0] * s + down[0] * drop, eave[1] + e[1] * s + down[1] * drop},
                pts[n++]);
        }
    glm_vec2_copy((vec2){eave[0] + down[0] * CUSP_DROP, eave[1] + down[1] * CUSP_DROP}, pts[n++]);
    kit_frame_extrude(kit, f, MAT_SIDING_DARK, pts, n, d0, d1);
}

void ornament_finial(Kit* kit, const KitFrame* f, int mat, float a, float y, float d, float h,
                     float drop) {
    // A square-ish base, a turned bulb, then a long taper to a point.
    const vec2 spire[] = {{0.0f, 0.0f},
                          {0.06f, 0.0f},
                          {0.06f, 0.1f * h},
                          {0.04f, 0.14f * h},
                          {0.075f, 0.26f * h},
                          {0.04f, 0.38f * h},
                          {0.028f, 0.45f * h},
                          {0.035f, 0.5f * h},
                          {0.018f, 0.56f * h},
                          {0.012f, 0.85f * h},
                          {0.0f, h}};
    kit_frame_lathe(kit, f, mat, a, d, y, spire, KIT_COUNT(spire), 10);
    if (drop <= 0.0f)
        return;
    const vec2 pendant[] = {{0.0f, 0.0f},
                            {0.05f, 0.0f},
                            {0.065f, 0.15f * drop},
                            {0.035f, 0.35f * drop},
                            {0.055f, 0.6f * drop},
                            {0.03f, 0.85f * drop},
                            {0.0f, drop}};
    kit_frame_lathe_on(kit, f, mat, (vec3){a, y, d}, (vec3){0.0f, -1.0f, 0.0f}, pendant,
                       KIT_COUNT(pendant), 10);
}

void ornament_cresting(Kit* kit, const KitFrame* f, float a0, float a1, float y, float d) {
    kit_frame_box(kit, f, MAT_IRON, a0, a1, y, y + 0.035f, d - 0.012f, d + 0.012f, false);
    // A spear with a cross-piece and a leaf either side of its head, every 0.3 m.
    static const vec2 SPIKE[] = {
        {-0.01f, 0.0f},   {0.01f, 0.0f},    {0.01f, 0.15f}, {0.045f, 0.17f}, {0.045f, 0.19f},
        {0.012f, 0.19f},  {0.03f, 0.24f},   {0.0f, 0.33f},  {-0.03f, 0.24f}, {-0.012f, 0.19f},
        {-0.045f, 0.19f}, {-0.045f, 0.17f}, {-0.01f, 0.15f}};
    const int n = (int)floorf((a1 - a0) / 0.3f);
    for (int i = 0; i <= n; i++) {
        const float a = a0 + (a1 - a0) * (float)i / (float)(n > 0 ? n : 1);
        vec2 pts[KIT_COUNT(SPIKE)];
        for (int k = 0; k < KIT_COUNT(SPIKE); k++)
            glm_vec2_copy((vec2){a + SPIKE[k][0], y + 0.035f + SPIKE[k][1]}, pts[k]);
        kit_frame_extrude(kit, f, MAT_IRON, pts, KIT_COUNT(SPIKE), d - 0.007f, d + 0.007f);
    }
}

void ornament_arch_board(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float spring,
                         float top, KitArchShape arch, float rise, float d) {
    const KitWall board = {.at = d,
                           .from = a0,
                           .to = a1,
                           .y0 = spring,
                           .y1 = top,
                           .thick = 0.05f,
                           .inner = 1,
                           .mat_inner = mat,
                           .mat_outer = mat,
                           .openings = {{a0, a1, spring, spring, arch, rise}},
                           .opening_count = 1};
    kit_frame_wall(kit, f, &board);
}

void ornament_balustrade(Kit* kit, const KitFrame* f, float a0, float a1, float y, float d) {
    const float rail = y + 0.86f;
    kit_frame_box(kit, f, MAT_SIDING_DARK, a0, a1, rail, rail + 0.06f, d - 0.045f, d + 0.045f,
                  false);
    kit_frame_box(kit, f, MAT_SIDING_DARK, a0, a1, y + 0.06f, y + 0.11f, d - 0.035f, d + 0.035f,
                  false);
    const vec2 turned[] = {{0.0f, 0.0f},    {0.028f, 0.0f}, {0.028f, 0.06f}, {0.016f, 0.1f},
                           {0.03f, 0.3f},   {0.016f, 0.5f}, {0.02f, 0.62f},  {0.026f, 0.7f},
                           {0.026f, 0.75f}, {0.0f, 0.75f}};
    const int n = (int)floorf((a1 - a0) / 0.14f);
    for (int i = 1; i < n; i++) {
        const float a = a0 + (a1 - a0) * (float)i / (float)n;
        kit_frame_lathe(kit, f, MAT_SIDING_DARK, a, d, y + 0.11f, turned, KIT_COUNT(turned), 8);
    }
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a0, a1, y, rail + 0.1f, d - 0.05f, d + 0.05f, true);
}

void ornament_post(Kit* kit, const KitFrame* f, float a, float d, float y0, float y1, float r) {
    // A square plinth, then a turned shaft with a ring at its middle and a capital.
    const float plinth = 0.16f, l = y1 - y0 - plinth;
    kit_frame_box(kit, f, MAT_SIDING_DARK, a - 1.5f * r, a + 1.5f * r, y0, y0 + plinth,
                  d - 1.5f * r, d + 1.5f * r, false);
    const vec2 turned[] = {{0.0f, 0.0f},
                           {1.2f * r, 0.0f},
                           {1.2f * r, 0.05f},
                           {r, 0.1f},
                           {0.85f * r, 0.45f * l},
                           {1.15f * r, 0.5f * l},
                           {0.9f * r, 0.55f * l},
                           {r, l - 0.2f},
                           {1.35f * r, l - 0.12f},
                           {1.35f * r, l - 0.06f},
                           {1.6f * r, l - 0.04f},
                           {1.6f * r, l},
                           {0.0f, l}};
    kit_frame_lathe(kit, f, MAT_SIDING_DARK, a, d, y0 + plinth, turned, KIT_COUNT(turned), 10);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - r, a + r, y0, y1, d - r, d + r, true);
}
