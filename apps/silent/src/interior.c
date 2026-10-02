#include <math.h>

#include "gothic.h"
#include "house.h"
#include "interior.h"
#include "layout.h"
#include "mats.h"
#include "ornament.h"

/*
 * The panelling, from the floor up: a skirting, a row of carved linenfold bays and, where the
 * room is tall enough, a rail and a row of blind tracery over them, a frieze of quatrefoils,
 * and a cap rail. Each band stands off the wall by its own depth; the carving is a card on a
 * mahogany backing.
 */
#define SKIRT    0.18f
#define MID_RAIL 0.08f
#define CAP      0.06f
#define BAY_W    0.55f
#define BACKING  (PANEL_DEPTH - 0.001f)
#define MIN_SPAN 0.25f // a run of panelling shorter than this between holes is left uncarved

#define BAY_H    (GOTHICS[GOTHIC_PANEL_LINENFOLD].size[1])
#define FRIEZE_H (GOTHICS[GOTHIC_FRIEZE].size[1])
#define FRIEZE_W (GOTHICS[GOTHIC_FRIEZE].size[0])

#define RUNNER_W   0.75f
#define RUNNER_END (GOTHICS[GOTHIC_RUNNER_END].size[1])
#define RUNNER_LEN (GOTHICS[GOTHIC_RUNNER].size[1])

typedef struct Span {
    float a0, a1;
} Span;

/*
 * The parts of a0..a1 clear of every hole reaching into the band y0..y1, in order. A hole
 * stops the panelling at its own edges: one with a casing round it is passed grown by it.
 */
static int clear_spans(float a0, float a1, float y0, float y1, const KitOpening* holes, int n,
                       Span* out) {
    Span blocked[KIT_MAX_OPENINGS + 1];
    int nb = 0;
    for (int i = 0; i < n; i++) {
        const KitOpening* o = &holes[i];
        if (o->bottom >= y1 || o->top + o->rise <= y0 || o->to <= a0 || o->from >= a1)
            continue;
        blocked[nb++] = (Span){fmaxf(o->from, a0), fminf(o->to, a1)};
        for (int j = nb - 1; j > 0 && blocked[j].a0 < blocked[j - 1].a0; j--) {
            const Span t = blocked[j];
            blocked[j] = blocked[j - 1];
            blocked[j - 1] = t;
        }
    }
    int count = 0;
    float cursor = a0;
    for (int i = 0; i < nb; i++) {
        if (blocked[i].a0 > cursor)
            out[count++] = (Span){cursor, blocked[i].a0};
        cursor = fmaxf(cursor, blocked[i].a1);
    }
    if (a1 > cursor)
        out[count++] = (Span){cursor, a1};
    return count;
}

/*
 * One band of the panelling, y up `h`, standing `proud` off the face along every clear span of
 * a0..a1, and carved with `card` (GOTHIC_COUNT for plain) at `width` a card. Returns its top.
 */
static float band(Kit* kit, const Facade* s, float a0, float a1, float y, float h, float proud,
                  GothicId card, float width, const KitOpening* holes, int n) {
    Span spans[KIT_MAX_OPENINGS + 2];
    const int count = clear_spans(a0, a1, y, y + h, holes, n, spans);
    for (int i = 0; i < count; i++) {
        kit_frame_box(kit, &s->f, MAT_MAHOGANY, spans[i].a0, spans[i].a1, y, y + h, s->face,
                      s->face + s->out * proud, false);
        if (card != GOTHIC_COUNT && spans[i].a1 - spans[i].a0 >= MIN_SPAN)
            kit_frame_card_row(kit, &s->f, MAT_CARVED, GOTHICS[card].uv, spans[i].a0, spans[i].a1,
                               y, y + h, s->face + s->out * PANEL_DEPTH, s->out, width);
    }
    return y + h;
}

/*
 * The panelling along a face from a0 to a1, standing on `floor` and stopping at every hole,
 * with the row of tracery over the linenfold when `tracery`. Returns the cap rail's top.
 */
static float wainscot(Kit* kit, const Facade* s, float a0, float a1, float floor, bool tracery,
                      const KitOpening* holes, int n) {
    float y = band(kit, s, a0, a1, floor, SKIRT, 0.035f, GOTHIC_COUNT, 0.0f, holes, n);
    y = band(kit, s, a0, a1, y, BAY_H, BACKING, GOTHIC_PANEL_LINENFOLD, BAY_W, holes, n);
    if (tracery) {
        y = band(kit, s, a0, a1, y, MID_RAIL, 0.03f, GOTHIC_COUNT, 0.0f, holes, n);
        y = band(kit, s, a0, a1, y, BAY_H, BACKING, GOTHIC_PANEL_TRACERY, BAY_W, holes, n);
    }
    y = band(kit, s, a0, a1, y, FRIEZE_H, BACKING, GOTHIC_FRIEZE, FRIEZE_W, holes, n);
    return band(kit, s, a0, a1, y, CAP, 0.05f, GOTHIC_COUNT, 0.0f, holes, n);
}

// A wall's openings as the panelling stops at them when every one is cased. Returns how many.
static int cased(const KitWall* w, KitOpening* out) {
    for (int i = 0; i < w->opening_count; i++)
        out[i] = kit_opening_grow(&w->openings[i], ORNAMENT_CASING_W);
    return w->opening_count;
}

/*
 * Dressed stone up a wall's inner face from y0 to the eave, a few centimetres thick, cut round
 * the wall's own openings -- a wall of its own standing on the face, so a lancet's arched
 * reveal is the stone's.
 */
static void lining(Kit* kit, const KitWall* w, float a0, float a1, float y0) {
    const Facade s = facade_inner(w);
    KitWall stone = *w;
    stone.at = s.face + s.out * 0.015f;
    stone.from = a0;
    stone.to = a1;
    stone.y0 = y0;
    stone.y1 = EAVE_Y;
    stone.thick = 0.03f;
    stone.inner = 1;
    stone.mat_inner = MAT_STONE;
    stone.mat_outer = MAT_STONE;
    kit_frame_wall(kit, &s.f, &stone);
}

/*
 * The hall: two rows of panelling down both sides and across the front either side of the
 * door, mahogany beams across its ceiling, and casings round its four doorways.
 */
static void hall(Kit* kit) {
    const KitWall* west = house_wall(HOUSE_WALL_HALL_WEST);
    const KitWall* east = house_wall(HOUSE_WALL_HALL_EAST);
    const KitWall* front = house_wall(HOUSE_WALL_FRONT);
    const KitWall* back = house_wall(HOUSE_WALL_GREAT_FRONT);
    KitOpening holes[KIT_MAX_OPENINGS];

    const Facade w = facade_inner(west), e = facade_of(east), f = facade_inner(front);
    int n = cased(west, holes);
    wainscot(kit, &w, BAND_Z0, BAND_Z1, FLOOR_Y, true, holes, n);
    n = cased(east, holes);
    wainscot(kit, &e, BAND_Z0, BAND_Z1, FLOOR_Y, true, holes, n);
    n = cased(front, holes);
    wainscot(kit, &f, HALL_IN_X0, HALL_IN_X1, FLOOR_Y, true, holes, n);
    ornament_casing(kit, &w, MAT_MAHOGANY, &west->openings[OPENING_PARLOUR_DOOR]);
    ornament_casing(kit, &e, MAT_MAHOGANY, &east->openings[OPENING_KITCHEN_DOOR]);
    ornament_casing(kit, &f, MAT_MAHOGANY, &front->openings[OPENING_FRONT_DOOR]);
    const Facade arch_hall = facade_inner(back), arch_great = facade_of(back);
    ornament_casing(kit, &arch_hall, MAT_MAHOGANY, &back->openings[OPENING_GREAT_ARCH]);
    ornament_casing(kit, &arch_great, MAT_MAHOGANY, &back->openings[OPENING_GREAT_ARCH]);

    for (int i = 0; i < 4; i++) {
        const float z = 10.6f + 0.9f * (float)i;
        kit_frame_box(kit, &KIT_WORLD, MAT_MAHOGANY, HALL_IN_X0, HALL_IN_X1, CEIL_Y - 0.14f, CEIL_Y,
                      z - 0.06f, z + 0.06f, false);
    }
}

/*
 * A truss across the great hall at z, of the hall's own timber: a tie beam from wall to wall
 * on posts standing on stone corbels, curved braces under its ends, principal rafters up the
 * roof to a king post at the ridge.
 */
static void truss(Kit* kit, float z) {
    const KitFrame* w = &KIT_WORLD;
    const float tie = EAVE_Y - 0.28f, depth = 0.09f;
    kit_frame_box(kit, w, MAT_MAHOGANY, GREAT_X0, GREAT_X1, tie, EAVE_Y, z - TRUSS_HALF,
                  z + TRUSS_HALF, false);
    const float ridge = house_roof_under_y(0.0f);
    kit_frame_box(kit, w, MAT_MAHOGANY, -0.08f, 0.08f, EAVE_Y, ridge, z - depth, z + depth, false);
    for (int s = -1; s <= 1; s += 2) {
        const float side = (float)s, wall = side > 0.0f ? GREAT_X1 : GREAT_X0;
        // The principal rafter, under the roof from the wall to the ridge.
        const float under = house_roof_under_y(wall), drop = 0.3f;
        const vec2 rafter[4] = {
            {wall, under}, {0.0f, ridge}, {0.0f, ridge - drop}, {wall, under - drop}};
        kit_frame_extrude(kit, w, MAT_MAHOGANY, rafter, 4, z - depth, z + depth);
        // The wall post and its corbel, and the brace curving from it up under the tie.
        const float post0 = EAVE_Y - 2.1f, inward = -side;
        kit_frame_box(kit, w, MAT_MAHOGANY, wall, wall + inward * 0.12f, post0, tie, z - 0.08f,
                      z + 0.08f, false);
        kit_frame_box(kit, w, MAT_STONE, wall, wall + inward * 0.18f, post0 - 0.22f, post0,
                      z - TRUSS_HALF, z + TRUSS_HALF, false);
        enum { SEG = 10 };
        vec2 brace[2 * (SEG + 1)];
        const float bx = wall + inward * 1.5f, by = EAVE_Y - 1.5f;
        int n = 0;
        for (int i = 0; i <= SEG; i++) {
            const float t = 0.5f * GLM_PIf * (float)i / SEG;
            glm_vec2_copy((vec2){bx - inward * 1.5f * cosf(t), by + 1.22f * sinf(t)}, brace[n++]);
        }
        for (int i = SEG; i >= 0; i--) {
            const float t = 0.5f * GLM_PIf * (float)i / SEG;
            glm_vec2_copy((vec2){bx - inward * 1.34f * cosf(t), by + 1.06f * sinf(t)}, brace[n++]);
        }
        kit_frame_extrude(kit, w, MAT_MAHOGANY, brace, n, z - 0.07f, z + 0.07f);
    }
}

/*
 * The great hall: two rows of panelling round its walls, stopping at the hearth and the
 * stair, dressed stone from the panelling to the eave -- and down to the floor behind the
 * stair -- two trusses, purlins and a ridge beam, and the Persian rug before the hearth.
 */
static void great_hall(Kit* kit) {
    const KitWall* west = house_wall(HOUSE_WALL_WEST);
    const KitWall* back = house_wall(HOUSE_WALL_BACK);
    const KitWall* east = house_wall(HOUSE_WALL_EAST);
    const KitWall* front = house_wall(HOUSE_WALL_GREAT_FRONT);

    const Facade w = facade_inner(west);
    const float top =
        wainscot(kit, &w, GREAT_Z0, GREAT_Z1, FLOOR_Y, true, west->openings, west->opening_count);
    lining(kit, west, GREAT_Z0, GREAT_Z1, top);

    // Up to the fireplace's jambs.
    KitWall dressed = *back;
    dressed.openings[dressed.opening_count++] =
        (KitOpening){HEARTH_X - HEARTH_HALF, HEARTH_X + HEARTH_HALF, FLOOR_Y, EAVE_Y};
    const Facade b = facade_inner(back);
    wainscot(kit, &b, GREAT_X0, GREAT_X1, FLOOR_Y, true, dressed.openings, dressed.opening_count);
    lining(kit, back, GREAT_X0, GREAT_X1, top);

    // The stair climbs the east wall, so the panelling stops either side of it and the stone
    // comes down to the floor behind it.
    dressed = *east;
    dressed.openings[dressed.opening_count++] =
        (KitOpening){GALLERY_Z1, STAIR_FOOT_Z, FLOOR_Y, FLOOR2_Y + 1.0f};
    const Facade e = facade_inner(east);
    wainscot(kit, &e, GREAT_Z0, GREAT_Z1, FLOOR_Y, true, dressed.openings, dressed.opening_count);
    lining(kit, east, GREAT_Z0, GALLERY_Z1, top);
    lining(kit, east, GALLERY_Z1, STAIR_FOOT_Z, FLOOR_Y);
    lining(kit, east, STAIR_FOOT_Z, GREAT_Z1, top);

    // Under the gallery, across the hall's front either side of the cased arch.
    KitOpening holes[KIT_MAX_OPENINGS];
    const int n = cased(front, holes);
    const Facade fr = facade_of(front);
    wainscot(kit, &fr, GREAT_X0, GREAT_X1, FLOOR_Y, true, holes, n);

    truss(kit, TRUSS_Z0);
    truss(kit, TRUSS_Z1);
    for (int s = -1; s <= 1; s += 2) {
        const float x = 2.45f * (float)s, under = house_roof_under_y(x);
        kit_frame_box(kit, &KIT_WORLD, MAT_MAHOGANY, x - 0.08f, x + 0.08f, under - 0.18f, under,
                      GREAT_Z0, GREAT_Z1, false);
    }
    const float ridge = house_roof_under_y(0.0f);
    kit_frame_box(kit, &KIT_WORLD, MAT_MAHOGANY, -0.1f, 0.1f, ridge - 0.25f, ridge, GREAT_Z0,
                  GREAT_Z1, false);
}

/*
 * A runner laid along an axis from `from` to `to` (x, z) on a floor at y: an end piece at each
 * end, fringed at its end, and middle pieces between, each card stretched to fit.
 */
static void runner_piece(Kit* kit, const vec3 corner, const vec3 across, const vec3 up,
                         GothicId id) {
    kit_frame_card(kit, &KIT_WORLD, MAT_PERSIAN, corner, across, up, GOTHICS[id].uv);
}

static void runner(Kit* kit, const vec2 from, const vec2 to, float y) {
    vec3 dir = {to[0] - from[0], 0.0f, to[1] - from[1]};
    const float len = glm_vec3_norm(dir);
    if (len < 2.0f * RUNNER_END)
        return;
    glm_vec3_scale(dir, 1.0f / len, dir);
    // Across, the way that makes a card along `dir` face up.
    const vec3 side = {-dir[2], 0.0f, dir[0]};
    const float lift = y + 0.006f;
    vec3 corner = {from[0] - side[0] * 0.5f * RUNNER_W, lift, from[1] - side[2] * 0.5f * RUNNER_W};
    vec3 across = {side[0] * RUNNER_W, 0.0f, side[2] * RUNNER_W};
    vec3 up = {dir[0] * RUNNER_END, 0.0f, dir[2] * RUNNER_END};
    runner_piece(kit, corner, across, up, GOTHIC_RUNNER_END);
    const float middle = len - 2.0f * RUNNER_END;
    const int n = (int)fmaxf(1.0f, roundf(middle / RUNNER_LEN));
    for (int i = 0; i < n; i++) {
        vec3 at = {0.0f, 0.0f, 0.0f};
        glm_vec3_copy(corner, at);
        glm_vec3_muladds(dir, RUNNER_END + middle * (float)i / (float)n, at);
        vec3 step = {dir[0] * middle / (float)n, 0.0f, dir[2] * middle / (float)n};
        runner_piece(kit, at, across, step, GOTHIC_RUNNER);
    }
    // The far end, turned round so its fringe is at the runner's end.
    vec3 far = {to[0] + side[0] * 0.5f * RUNNER_W, lift, to[1] + side[2] * 0.5f * RUNNER_W};
    vec3 back_across = {-across[0], 0.0f, -across[2]}, back_up = {-up[0], 0.0f, -up[2]};
    runner_piece(kit, far, back_across, back_up, GOTHIC_RUNNER_END);
}

// The rug: 3 m across by 4 m long, laid up to the hearth's stone with its middle on the hall's
// axis.
static void rug(Kit* kit) {
    const float w = GOTHICS[GOTHIC_RUG].size[0], l = GOTHICS[GOTHIC_RUG].size[1];
    const float z0 = GREAT_Z1 - HEARTH_FRONT - 0.03f - l;
    runner_piece(kit, (vec3){HEARTH_X + 0.5f * w, FLOOR_Y + 0.008f, z0}, (vec3){-w, 0.0f, 0.0f},
                 (vec3){0.0f, 0.0f, l}, GOTHIC_RUG);
}

/*
 * The stair dressed: a runner up its middle, over each tread and up each riser, held by a brass
 * rod at every step; a balustrade up its open side between newels at its foot and its head;
 * and the gallery's balustrade from that head to the west wall.
 */
static void stair_dressing(Kit* kit) {
    const float x0 = 0.5f * (STAIR_X0 + GREAT_X1) - 0.35f, x1 = x0 + 0.7f;
    const float* uv = GOTHICS[GOTHIC_RUNNER].uv;
    float along = 0.0f; // metres of runner laid, for where in its picture each piece comes from
    for (int i = 1; i <= STAIR_RISERS; i++) {
        const float y_lo = FLOOR_Y + (float)(i - 1) * STAIR_RISE, y_hi = y_lo + STAIR_RISE;
        const float z_front = STAIR_FOOT_Z - (float)(i - 1) * STAIR_GOING;
        const float pieces[2] = {STAIR_RISE, i < STAIR_RISERS ? STAIR_GOING : 0.0f};
        for (int p = 0; p < 2; p++) {
            if (pieces[p] <= 0.0f)
                continue;
            float v = fmodf(along, RUNNER_LEN);
            if (v + pieces[p] > RUNNER_LEN)
                v = 0.0f;
            const float slice[4] = {uv[0], uv[1] + (uv[3] - uv[1]) * v / RUNNER_LEN, uv[2],
                                    uv[1] + (uv[3] - uv[1]) * (v + pieces[p]) / RUNNER_LEN};
            if (p == 0)
                kit_frame_card(kit, &KIT_WORLD, MAT_PERSIAN, (vec3){x0, y_lo, z_front + 0.004f},
                               (vec3){x1 - x0, 0.0f, 0.0f}, (vec3){0.0f, STAIR_RISE, 0.0f}, slice);
            else
                kit_frame_card(kit, &KIT_WORLD, MAT_PERSIAN, (vec3){x0, y_hi + 0.004f, z_front},
                               (vec3){x1 - x0, 0.0f, 0.0f}, (vec3){0.0f, 0.0f, -STAIR_GOING},
                               slice);
            along += pieces[p];
        }
        if (i < STAIR_RISERS) {
            const float z = z_front - STAIR_GOING + 0.012f;
            const vec3 rod[2] = {{x0 - 0.04f, y_hi + 0.012f, z}, {x1 + 0.04f, y_hi + 0.012f, z}};
            kit_frame_pipe(kit, &KIT_WORLD, MAT_BRASS, rod, 2, 0.007f, 8);
        }
    }

    // The balustrade up the open side: the rail along the rake, a baluster on every tread.
    const float bx = STAIR_X0 + 0.05f, rail = 0.88f;
    const float foot_z = STAIR_FOOT_Z - 0.5f * STAIR_GOING, foot_y = FLOOR_Y + STAIR_RISE;
    ornament_post(kit, &KIT_WORLD, bx, foot_z, foot_y, foot_y + rail + 0.35f, 0.065f);
    ornament_post(kit, &KIT_WORLD, bx, GALLERY_Z1 - 0.05f, FLOOR2_Y, FLOOR2_Y + rail + 0.35f,
                  0.065f);
    const KitFrame along_z = {{bx, 0.0f, 0.0f}, -0.5f * GLM_PIf}; // a is z, d is -x
    const float top_y = FLOOR2_Y + rail;
    const vec2 handrail[4] = {{GALLERY_Z1 - 0.05f, top_y},
                              {foot_z, foot_y + rail},
                              {foot_z, foot_y + rail + 0.07f},
                              {GALLERY_Z1 - 0.05f, top_y + 0.07f}};
    kit_frame_extrude(kit, &along_z, MAT_MAHOGANY, handrail, 4, -0.035f, 0.035f);
    const vec2 turned[] = {{0.0f, 0.0f},    {0.026f, 0.0f},  {0.026f, 0.05f}, {0.015f, 0.09f},
                           {0.028f, 0.28f}, {0.015f, 0.47f}, {0.024f, 0.6f},  {0.0f, 0.62f}};
    for (int i = 2; i < STAIR_RISERS; i++) {
        const float z = STAIR_FOOT_Z - ((float)i - 0.5f) * STAIR_GOING;
        const float y = FLOOR_Y + (float)i * STAIR_RISE;
        const float t = (z - foot_z) / (GALLERY_Z1 - 0.05f - foot_z);
        const float to = foot_y + rail + t * (top_y - foot_y - rail);
        vec2 scaled[KIT_COUNT(turned)];
        for (int k = 0; k < KIT_COUNT(turned); k++)
            glm_vec2_copy((vec2){turned[k][0], turned[k][1] * (to - y) / 0.62f}, scaled[k]);
        kit_frame_lathe(kit, &KIT_WORLD, MAT_MAHOGANY, bx, z, y, scaled, KIT_COUNT(scaled), 8);
        kit_frame_box(kit, &KIT_WORLD, KIT_COLLIDER_ONLY, bx - 0.04f, bx + 0.04f, y, to + 0.1f,
                      z - 0.5f * STAIR_GOING, z + 0.5f * STAIR_GOING, true);
    }

    // The gallery's, from the stair's head newel to a half-post on the west wall.
    ornament_balustrade(kit, &KIT_WORLD, GREAT_X0 + 0.05f, STAIR_X0 - 0.03f, FLOOR2_Y,
                        GALLERY_Z1 - 0.05f);
    ornament_post(kit, &KIT_WORLD, GREAT_X0 + 0.07f, GALLERY_Z1 - 0.05f, FLOOR2_Y,
                  FLOOR2_Y + rail + 0.35f, 0.065f);
}

// The gallery: one row of panelling along the great hall's front wall, round its three
// doorways, and their casings.
static void gallery(Kit* kit) {
    const KitWall* front = house_wall(HOUSE_WALL_GREAT_FRONT);
    KitOpening holes[KIT_MAX_OPENINGS];
    const int n = cased(front, holes);
    const Facade f = facade_of(front);
    wainscot(kit, &f, GREAT_X0, GREAT_X1, FLOOR2_Y, false, holes, n);
    for (int i = OPENING_STUDY_DOOR; i <= OPENING_BEDROOM_DOOR; i++)
        ornament_casing(kit, &f, MAT_MAHOGANY, &front->openings[i]);
}

void interior_build(Kit* kit) {
    hall(kit);
    great_hall(kit);
    gallery(kit);
    stair_dressing(kit);
    rug(kit);
    const float hall_x = 0.5f * (HALL_X0 + HALL_X1);
    runner(kit, (vec2){hall_x, HOUSE_FRONT_Z + 0.35f}, (vec2){hall_x, KITCHEN_BACK_Z - 0.1f},
           FLOOR_Y);
    const float gallery_z = 0.5f * (KITCHEN_BACK_Z + GALLERY_Z1);
    runner(kit, (vec2){GREAT_X0 + 0.4f, gallery_z}, (vec2){STAIR_X0 - 0.2f, gallery_z}, FLOOR2_Y);
}
