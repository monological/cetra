#include <math.h>

#include "candles.h"
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
#define LINING   0.03f // the dressed stone's thickness over a wall's face

// A sconce's backplate is centred this far over the panelling's cap, or on the stair this far
// over the line of its nosings, which clears a head under the plate.
#define SCONCE_OVER        0.35f
#define SCONCE_ABOVE_STAIR 2.1f

#define BAY_H    (GOTHICS[GOTHIC_PANEL_LINENFOLD].size[1])
#define FRIEZE_H (GOTHICS[GOTHIC_FRIEZE].size[1])
#define FRIEZE_W (GOTHICS[GOTHIC_FRIEZE].size[0])

#define RUNNER_W   0.75f
#define RUNNER_END (GOTHICS[GOTHIC_RUNNER_END].size[1])
#define RUNNER_LEN (GOTHICS[GOTHIC_RUNNER].size[1])

// A point in the hall and one in the great hall, in plan: which side of a wall a room's
// dressing goes on is the side facing its point.
#define HALL_AT_X  (0.5f * (HALL_X0 + HALL_X1))
#define HALL_AT_Z  (0.5f * (BAND_Z0 + BAND_Z1))
#define GREAT_AT_Z (0.5f * (GREAT_Z0 + GREAT_Z1))

/*
 * One band of the panelling, y up `h`, standing `proud` off the face along a0..a1 where no hole
 * reaches into it, and carved with `card` (GOTHIC_COUNT for plain) at `width` a card. A hole
 * stops the panelling at its own edges: one with a casing round it is passed grown by it.
 * Returns its top.
 */
static float band(Kit* kit, const Facade* s, float a0, float a1, float y, float h, float proud,
                  GothicId card, float width, const KitOpening* holes, int n) {
    vec2 blocked[KIT_MAX_OPENINGS + 1] = {{0.0f}}, spans[KIT_MAX_OPENINGS + 2];
    int nb = 0;
    for (int i = 0; i < n; i++)
        if (holes[i].bottom < y + h && kit_opening_crown(&holes[i]) > y)
            glm_vec2_copy((vec2){holes[i].from, holes[i].to}, blocked[nb++]);
    const int count = kit_clear_spans(a0, a1, blocked, nb, spans);
    for (int i = 0; i < count; i++) {
        facade_box(kit, s, MAT_MAHOGANY, spans[i][0], spans[i][1], y, y + h, proud);
        if (card != GOTHIC_COUNT && spans[i][1] - spans[i][0] >= MIN_SPAN)
            kit_frame_card_row(kit, &s->f, MAT_GOTHIC, GOTHICS[card].uv, spans[i][0], spans[i][1],
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
 * the wall's own openings -- a panel standing on the face, so a lancet's arched reveal is the
 * stone's. It runs on behind the hearth, buried in its stone.
 */
static void lining(Kit* kit, const KitWall* w, float a0, float a1, float y0) {
    const Facade s = facade_toward(w, 0.0f, GREAT_AT_Z);
    KitWall stone = *w;
    stone.at = s.face + s.out * 0.5f * LINING;
    stone.from = a0;
    stone.to = a1;
    stone.y0 = y0;
    stone.y1 = EAVE_Y;
    stone.thick = LINING;
    kit_frame_panel(kit, &s.f, MAT_STONE, &stone);
}

// The face of a wall's stone lining as the great hall sees it.
static Facade lined(const KitWall* w) {
    Facade s = facade_toward(w, 0.0f, GREAT_AT_Z);
    s.face += s.out * LINING;
    return s;
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

    const Facade w = facade_toward(west, HALL_AT_X, HALL_AT_Z);
    const Facade e = facade_toward(east, HALL_AT_X, HALL_AT_Z);
    const Facade f = facade_toward(front, HALL_AT_X, HALL_AT_Z);
    int n = cased(west, holes);
    wainscot(kit, &w, BAND_Z0, BAND_Z1, FLOOR_Y, true, holes, n);
    n = cased(east, holes);
    wainscot(kit, &e, BAND_Z0, BAND_Z1, FLOOR_Y, true, holes, n);
    n = cased(front, holes);
    wainscot(kit, &f, HALL_IN_X0, HALL_IN_X1, FLOOR_Y, true, holes, n);
    ornament_casing(kit, &w, MAT_MAHOGANY, &west->openings[OPENING_PARLOUR_DOOR]);
    ornament_casing(kit, &e, MAT_MAHOGANY, &east->openings[OPENING_KITCHEN_DOOR]);
    ornament_casing(kit, &f, MAT_MAHOGANY, &front->openings[OPENING_FRONT_DOOR]);
    const Facade arch_hall = facade_toward(back, HALL_AT_X, HALL_AT_Z);
    const Facade arch_great = facade_toward(back, HALL_AT_X, GREAT_AT_Z);
    ornament_casing(kit, &arch_hall, MAT_MAHOGANY, &back->openings[OPENING_GREAT_ARCH]);
    ornament_casing(kit, &arch_great, MAT_MAHOGANY, &back->openings[OPENING_GREAT_ARCH]);

    enum { BEAMS = 4 };
    for (int i = 0; i < BEAMS; i++) {
        const float z = BAND_Z0 + ((float)i + 0.5f) * (BAND_Z1 - BAND_Z0) / (float)BEAMS;
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
        const vec2 c = {wall + inward * 1.5f, EAVE_Y - 1.5f};
        const int n = kit_arc_band(c, -inward * 1.5f, 1.22f, -inward * 1.34f, 1.06f, 0.0f,
                                   0.5f * GLM_PIf, SEG, brace);
        kit_frame_extrude(kit, w, MAT_MAHOGANY, brace, n, z - 0.07f, z + 0.07f);
    }
}

/*
 * The great hall: two rows of panelling round its walls, stopping at the hearth and the
 * stair, dressed stone from the panelling to the eave -- and down to the floor behind the
 * stair -- two trusses, purlins and a ridge beam, a sconce on the west wall under each truss,
 * and the Persian rug before the hearth.
 */
static void great_hall(Kit* kit) {
    const KitWall* west = house_wall(HOUSE_WALL_WEST);
    const KitWall* back = house_wall(HOUSE_WALL_BACK);
    const KitWall* east = house_wall(HOUSE_WALL_EAST);
    const KitWall* front = house_wall(HOUSE_WALL_GREAT_FRONT);

    const Facade w = facade_toward(west, 0.0f, GREAT_AT_Z);
    const float top =
        wainscot(kit, &w, GREAT_Z0, GREAT_Z1, FLOOR_Y, true, west->openings, west->opening_count);
    lining(kit, west, GREAT_Z0, GREAT_Z1, top);
    // A sconce on the stone under each truss, where its post comes down the wall between the
    // lancets' bays.
    const Facade stone = lined(west);
    candle_sconce(kit, &stone, TRUSS_Z0, top + SCONCE_OVER, 0.15f);
    candle_sconce(kit, &stone, TRUSS_Z1, top + SCONCE_OVER, 0.13f);

    // Up to the fireplace's jambs.
    KitWall dressed = *back;
    dressed.openings[dressed.opening_count++] =
        (KitOpening){HEARTH_X - HEARTH_HALF, HEARTH_X + HEARTH_HALF, FLOOR_Y, EAVE_Y};
    const Facade b = facade_toward(back, 0.0f, GREAT_AT_Z);
    wainscot(kit, &b, GREAT_X0, GREAT_X1, FLOOR_Y, true, dressed.openings, dressed.opening_count);
    lining(kit, back, GREAT_X0, GREAT_X1, top);

    // The stair climbs the east wall, so the panelling stops either side of it and the stone
    // comes down to the floor behind it.
    dressed = *east;
    dressed.openings[dressed.opening_count++] =
        (KitOpening){GALLERY_Z1, STAIR_FOOT_Z, FLOOR_Y, FLOOR2_Y + 1.0f};
    const Facade e = facade_toward(east, 0.0f, GREAT_AT_Z);
    wainscot(kit, &e, GREAT_Z0, GREAT_Z1, FLOOR_Y, true, dressed.openings, dressed.opening_count);
    lining(kit, east, GREAT_Z0, GALLERY_Z1, top);
    lining(kit, east, GALLERY_Z1, STAIR_FOOT_Z, FLOOR_Y);
    lining(kit, east, STAIR_FOOT_Z, GREAT_Z1, top);

    // Under the gallery, across the hall's front either side of the cased arch.
    KitOpening holes[KIT_MAX_OPENINGS];
    const int n = cased(front, holes);
    const Facade fr = facade_toward(front, 0.0f, GREAT_AT_Z);
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
    kit_frame_card(kit, &KIT_WORLD, MAT_GOTHIC, corner, across, up, GOTHICS[id].uv);
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
 * The next `len` of the stair's runner, from `corner` across `across` and along `up`: the
 * stretch of the runner's picture after what is already laid, or from its start where the rest
 * of the picture would not cover it.
 */
static void stair_runner(Kit* kit, float* laid, float len, const vec3 corner, const vec3 across,
                         const vec3 up) {
    const float* uv = GOTHICS[GOTHIC_RUNNER].uv;
    float v = fmodf(*laid, RUNNER_LEN);
    if (v + len > RUNNER_LEN)
        v = 0.0f;
    const float slice[4] = {uv[0], uv[1] + (uv[3] - uv[1]) * v / RUNNER_LEN, uv[2],
                            uv[1] + (uv[3] - uv[1]) * (v + len) / RUNNER_LEN};
    kit_frame_card(kit, &KIT_WORLD, MAT_GOTHIC, corner, across, up, slice);
    *laid += len;
}

/*
 * The stair dressed: a runner up its middle, over each tread and up each riser, held by a brass
 * rod at every step; a balustrade up its open side between newels at its foot and its head;
 * the gallery's balustrade from that head to the west wall; and a sconce either side of the
 * lancet over the flight.
 */
static void stair_dressing(Kit* kit) {
    const float x0 = 0.5f * (STAIR_X0 + GREAT_X1) - 0.35f, x1 = x0 + 0.7f;
    const vec3 across = {x1 - x0, 0.0f, 0.0f};
    float laid = 0.0f;
    for (int i = 1; i <= STAIR_RISERS; i++) {
        const float y_lo = FLOOR_Y + (float)(i - 1) * STAIR_RISE, y_hi = y_lo + STAIR_RISE;
        const float z_front = STAIR_FOOT_Z - (float)(i - 1) * STAIR_GOING;
        stair_runner(kit, &laid, STAIR_RISE, (vec3){x0, y_lo, z_front + 0.004f}, across,
                     (vec3){0.0f, STAIR_RISE, 0.0f});
        // The last riser lands on the gallery: no tread of its own, and no rod.
        if (i < STAIR_RISERS) {
            stair_runner(kit, &laid, STAIR_GOING, (vec3){x0, y_hi + 0.004f, z_front}, across,
                         (vec3){0.0f, 0.0f, -STAIR_GOING});
            const float z = z_front - STAIR_GOING + 0.012f;
            const vec3 rod[2] = {{x0 - 0.04f, y_hi + 0.012f, z}, {x1 + 0.04f, y_hi + 0.012f, z}};
            kit_frame_pipe(kit, &KIT_WORLD, MAT_BRASS, rod, 2, 0.007f, 8);
        }
    }

    // The balustrade up the open side: the rail along the rake, a baluster on every tread.
    const float bx = STAIR_X0 + 0.05f, rail = 0.88f, newel = rail + 0.35f;
    const float foot_z = STAIR_FOOT_Z - 0.5f * STAIR_GOING, foot_y = FLOOR_Y + STAIR_RISE;
    const float head_z = GALLERY_Z1 - 0.05f; // the gallery's balustrade line, and the stair's head
    ornament_post(kit, &KIT_WORLD, MAT_MAHOGANY, bx, foot_z, foot_y, foot_y + newel, 0.065f);
    ornament_post(kit, &KIT_WORLD, MAT_MAHOGANY, bx, head_z, FLOOR2_Y, FLOOR2_Y + newel, 0.065f);
    const KitFrame along_z = {{bx, 0.0f, 0.0f}, -0.5f * GLM_PIf}; // a is z, d is -x
    const float top_y = FLOOR2_Y + rail;
    const vec2 handrail[4] = {{head_z, top_y},
                              {foot_z, foot_y + rail},
                              {foot_z, foot_y + rail + 0.07f},
                              {head_z, top_y + 0.07f}};
    kit_frame_extrude(kit, &along_z, MAT_MAHOGANY, handrail, 4, -0.035f, 0.035f);
    for (int i = 2; i < STAIR_RISERS; i++) {
        const float z = STAIR_FOOT_Z - ((float)i - 0.5f) * STAIR_GOING;
        const float y = FLOOR_Y + (float)i * STAIR_RISE;
        const float t = (z - foot_z) / (head_z - foot_z);
        const float to = foot_y + rail + t * (top_y - foot_y - rail);
        ornament_baluster(kit, &KIT_WORLD, MAT_MAHOGANY, bx, y, z, to - y);
        kit_frame_box(kit, &KIT_WORLD, KIT_COLLIDER_ONLY, bx - 0.04f, bx + 0.04f, y, to + 0.1f,
                      z - 0.5f * STAIR_GOING, z + 0.5f * STAIR_GOING, true);
    }

    // The gallery's, from the stair's head newel to a half-post on the west wall.
    ornament_balustrade(kit, &KIT_WORLD, MAT_MAHOGANY, GREAT_X0 + 0.05f, STAIR_X0 - 0.03f, FLOOR2_Y,
                        head_z);
    ornament_post(kit, &KIT_WORLD, MAT_MAHOGANY, GREAT_X0 + 0.07f, head_z, FLOOR2_Y,
                  FLOOR2_Y + newel, 0.065f);

    // A sconce on the stone either side of the lancet over the stair, the upper one far enough
    // down the flight to keep off the near truss's corbel.
    const KitWall* east = house_wall(HOUSE_WALL_EAST);
    const Facade stone = lined(east);
    const KitOpening* lancet = &east->openings[OPENING_STAIR_LANCET];
    const float mid = 0.5f * (lancet->from + lancet->to), spread = 1.25f;
    for (int s = -1; s <= 1; s += 2) {
        const float z = mid + (float)s * spread;
        const float nosing = FLOOR_Y + (STAIR_FOOT_Z - z) / STAIR_GOING * STAIR_RISE;
        candle_sconce(kit, &stone, z, nosing + SCONCE_ABOVE_STAIR, s < 0 ? 0.12f : 0.16f);
    }
}

/*
 * The gallery: one row of panelling along the great hall's front wall, round its three
 * doorways, and their casings; and a sconce either side of the study's doorway over the
 * panelling, framing the one door off the gallery that stands open.
 */
static void gallery(Kit* kit) {
    const KitWall* front = house_wall(HOUSE_WALL_GREAT_FRONT);
    KitOpening holes[KIT_MAX_OPENINGS];
    const int n = cased(front, holes);
    const Facade f = facade_toward(front, 0.0f, GREAT_AT_Z);
    const float top = wainscot(kit, &f, GREAT_X0, GREAT_X1, FLOOR2_Y, false, holes, n);
    for (int i = OPENING_STUDY_DOOR; i <= OPENING_BEDROOM_DOOR; i++)
        ornament_casing(kit, &f, MAT_MAHOGANY, &front->openings[i]);

    const KitOpening* door = &front->openings[OPENING_STUDY_DOOR];
    const float mid = 0.5f * (door->from + door->to);
    const float off = 0.5f * (door->to - door->from) + ORNAMENT_CASING_W + 0.25f;
    for (int s = -1; s <= 1; s += 2)
        candle_sconce(kit, &f, mid + (float)s * off, top + SCONCE_OVER, 0.15f);
}

void interior_build(Kit* kit) {
    hall(kit);
    great_hall(kit);
    gallery(kit);
    stair_dressing(kit);
    rug(kit);
    runner(kit, (vec2){HALL_AT_X, HOUSE_FRONT_Z + 0.35f}, (vec2){HALL_AT_X, KITCHEN_BACK_Z - 0.1f},
           FLOOR_Y);
    const float gallery_z = 0.5f * (KITCHEN_BACK_Z + GALLERY_Z1);
    runner(kit, (vec2){GREAT_X0 + 0.4f, gallery_z}, (vec2){STAIR_X0 - 0.2f, gallery_z}, FLOOR2_Y);
}
