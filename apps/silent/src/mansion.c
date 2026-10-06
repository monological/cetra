#include <math.h>

#include "candles.h"
#include "gothic.h"
#include "house.h"
#include "mansion.h"
#include "mats.h"
#include "ornament.h"

/*
 * The mansion's front row (spec 13.25). Where the player's house has its kitchen, the Gothic
 * house has a dining room: a refectory table laid for a meal nobody came back to, high-backed
 * chairs, an iron candelabra burning on it, a sideboard of pewter, two portraits over the
 * panelling, and a seat under the pointed window. The hall keeps its panelling, and a suit of
 * armour stands where the player's house has its clock, by a candle stand.
 *
 * Heights are above the floor, and the room's things keep clear of the irradiance probes'
 * centres: x 0.415, 1.625, 2.835 and 4.045, z 10.605, 11.815 and 13.025, the lowest 0.37 above
 * the floor. A probe inside solid geometry darkens everything it lights.
 */

#define TABLE_HA  1.15f // half its length, along x
#define TABLE_HD  0.45f // half its width
#define TABLE_TOP 0.80f
// The trestles' middles either side of the table's: far enough out to keep the board standing
// across the table clear of the probe column at x 1.625.
#define TRESTLE_A 1.03f

#define CANDELABRA_H 0.42f // its foot to the middle socket's lip
#define SIDE_H       0.32f // and to the two side sockets' lips
#define ARM          0.2f  // how far out the side sockets stand
#define SOCKET_H     0.036f
#define MID_WAX      0.2f

// The rug under the table, its picture turned to run along the room.
#define RUG_X0 (TABLE_X - 2.0f)
#define RUG_X1 (TABLE_X + 2.0f)
#define RUG_Z0 (TABLE_Z - 1.5f)
#define RUG_Z1 (TABLE_Z + 1.5f)

static const vec2 PLATE[] = {
    {0.0f, 0.003f},  {0.07f, 0.003f},  {0.072f, 0.0f},   {0.08f, 0.0f},   {0.085f, 0.005f},
    {0.13f, 0.014f}, {0.132f, 0.017f}, {0.129f, 0.018f}, {0.086f, 0.01f}, {0.0f, 0.009f},
};

static const vec2 GOBLET[] = {
    {0.0f, 0.0f},    {0.034f, 0.0f},  {0.033f, 0.004f}, {0.01f, 0.012f},
    {0.006f, 0.03f}, {0.006f, 0.09f}, {0.011f, 0.1f},   {0.034f, 0.116f},
    {0.039f, 0.16f}, {0.036f, 0.16f}, {0.031f, 0.119f}, {0.0f, 0.108f},
};

// A chair post's turned finial.
static const vec2 FINIAL[] = {{0.0f, 0.0f},    {0.026f, 0.0f},   {0.026f, 0.01f}, {0.018f, 0.02f},
                              {0.03f, 0.045f}, {0.012f, 0.075f}, {0.0f, 0.09f}};

// The pans the candelabra's candles stand in: a dished pan and a short socket, iron.
static const vec2 SOCKET[] = {
    {0.0f, 0.0f},       {0.008f, 0.0f},   {0.04f, 0.008f}, {0.042f, 0.014f},
    {0.036f, 0.014f},   {0.012f, 0.012f}, {0.012f, 0.03f}, {0.016f, 0.035f},
    {0.013f, SOCKET_H}, {0.011f, 0.028f}, {0.0f, 0.028f},
};

/*
 * A refectory table: a thick board on two trestles, each a board standing across the table,
 * waisted, on a foot, and a stretcher between them low down, wedged through each.
 */
static void table(Kit* kit) {
    const KitFrame f = {{TABLE_X, FLOOR_Y, TABLE_Z}, 0.0f};
    const float under = TABLE_TOP - 0.05f;
    kit_frame_box(kit, &f, MAT_MAHOGANY, -TABLE_HA, TABLE_HA, under, TABLE_TOP, -TABLE_HD, TABLE_HD,
                  false);
    for (int s = -1; s <= 1; s += 2) {
        const float d = (float)s * (TABLE_HD - 0.08f);
        kit_frame_box(kit, &f, MAT_MAHOGANY, -TABLE_HA + 0.1f, TABLE_HA - 0.1f, under - 0.08f,
                      under, d - 0.015f, d + 0.015f, false);
    }
    // Each trestle in a frame turned a quarter, so its outline is drawn across the table.
    const vec2 board[] = {{-0.36f, 0.08f}, {0.36f, 0.08f},          {0.16f, 0.12f},
                          {0.11f, 0.42f},  {0.27f, under - 0.03f},  {0.32f, under},
                          {-0.32f, under}, {-0.27f, under - 0.03f}, {-0.11f, 0.42f},
                          {-0.16f, 0.12f}};
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s * TRESTLE_A;
        const KitFrame across = {{TABLE_X + a, FLOOR_Y, TABLE_Z}, 0.5f * GLM_PIf};
        kit_frame_extrude(kit, &across, MAT_MAHOGANY, board, KIT_COUNT(board), -0.035f, 0.035f);
        kit_frame_box(kit, &f, MAT_MAHOGANY, a - 0.06f, a + 0.06f, 0.0f, 0.08f, -0.38f, 0.38f,
                      false);
        // The stretcher's wedge, through the trestle on its outer side.
        const float out = a + (float)s * 0.07f;
        kit_frame_box(kit, &f, MAT_MAHOGANY, out - 0.015f, out + 0.015f, 0.2f, 0.34f, -0.02f, 0.02f,
                      false);
    }
    kit_frame_box(kit, &f, MAT_MAHOGANY, -TRESTLE_A - 0.1f, TRESTLE_A + 0.1f, 0.24f, 0.31f, -0.035f,
                  0.035f, false);
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -TABLE_HA, TABLE_HA, 0.0f, TABLE_TOP, -TABLE_HD,
                  TABLE_HD, true);
}

/*
 * A high-backed chair at (x, z), its sitter facing +d of `yaw`: a leather seat on four legs,
 * the back two running up as posts to finials, a panel of carved tracery between them under a
 * top rail.
 */
static void chair(Kit* kit, float x, float z, float yaw) {
    const KitFrame f = {{x, FLOOR_Y, z}, yaw};
    const float s = 0.23f, leg = 0.022f, seat = DINING_SEAT_TOP, back = 1.3f;
    kit_frame_box(kit, &f, MAT_MAHOGANY, -s, s, seat - 0.07f, seat - 0.025f, -s, s, false);
    kit_frame_box(kit, &f, MAT_LEATHER, -s + 0.015f, s - 0.015f, seat - 0.025f, seat, -s + 0.05f,
                  s - 0.01f, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? s - leg : -s + leg, d = (i & 2) ? s - leg : -s + leg;
        const bool post = !(i & 2);
        kit_frame_box(kit, &f, MAT_MAHOGANY, a - leg, a + leg, 0.0f, post ? back : seat - 0.07f,
                      d - leg, d + leg, false);
        if (post)
            kit_frame_lathe(kit, &f, MAT_MAHOGANY, a, d, back, FINIAL, KIT_COUNT(FINIAL), 8);
    }
    const float d = -s + leg;
    kit_frame_box(kit, &f, MAT_MAHOGANY, -s + leg, s - leg, back - 0.08f, back - 0.01f, d - 0.018f,
                  d + 0.018f, false);
    kit_frame_box(kit, &f, MAT_MAHOGANY, -s + 2.0f * leg, s - 2.0f * leg, seat + 0.04f,
                  back - 0.08f, d - 0.01f, d + 0.004f, false);
    kit_frame_card_rect(kit, &f, MAT_GOTHIC, GOTHICS[GOTHIC_PANEL_TRACERY].uv,
                        -s + 2.0f * leg + 0.015f, s - 2.0f * leg - 0.015f, seat + 0.07f,
                        back - 0.11f, d + 0.005f, 1.0f);
    kit_frame_box(kit, &f, MAT_MAHOGANY, -s + leg, s - leg, 0.12f, 0.16f, -s + leg - 0.01f,
                  -s + leg + 0.01f, false);
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -s, s, 0.0f, seat, -s, s, true);
}

// A socket and its pan on y at (a, d), and a taper in it burnt down to `wax`.
static void socket(Kit* kit, const KitFrame* f, float a, float y, float d, float wax) {
    kit_frame_lathe(kit, f, MAT_IRON, a, d, y, SOCKET, KIT_COUNT(SOCKET), 12);
    candle_taper(kit, f, a, y + 0.028f, d, wax);
}

/*
 * A black iron candelabra in the middle of the table: a stepped foot, a stem with a knop, and
 * two arms swept out and up from the knop to sockets either side of the middle one, the three
 * burnt down unevenly.
 */
static void candelabra(Kit* kit) {
    const KitFrame f = {{TABLE_X, FLOOR_Y, TABLE_Z}, 0.0f};
    const float y = TABLE_TOP, knop = 0.22f;
    const vec2 stem[] = {{0.0f, 0.0f},
                         {0.09f, 0.0f},
                         {0.09f, 0.015f},
                         {0.06f, 0.03f},
                         {0.03f, 0.06f},
                         {0.016f, 0.08f},
                         {0.014f, knop - 0.02f},
                         {0.032f, knop},
                         {0.014f, knop + 0.02f},
                         {0.012f, CANDELABRA_H - 0.02f},
                         {0.0f, CANDELABRA_H - 0.02f}};
    kit_frame_lathe(kit, &f, MAT_IRON, 0.0f, 0.0f, y, stem, KIT_COUNT(stem), 12);
    socket(kit, &f, 0.0f, y + CANDELABRA_H - 0.02f, 0.0f, MID_WAX);

    enum { POINTS = 12 };
    for (int s = -1; s <= 1; s += 2) {
        vec3 arm[POINTS] = {{0.0f}};
        const float tip = (float)s * ARM;
        for (int i = 0; i < POINTS; i++) {
            const float t = (float)i / (float)(POINTS - 1);
            glm_vec3_copy(
                (vec3){glm_bezier(t, 0.0f, 0.6f * tip, tip, tip),
                       y + glm_bezier(t, knop, knop - 0.06f, knop - 0.02f, SIDE_H - 0.01f), 0.0f},
                arm[i]);
        }
        kit_frame_pipe(kit, &f, MAT_IRON, arm, POINTS, 0.008f, 8);
        socket(kit, &f, tip, y + SIDE_H - 0.01f, 0.0f, s < 0 ? 0.14f : 0.09f);
    }
}

void mansion_candelabra_flame(vec3 out) {
    const float local[3] = {
        TABLE_X, FLOOR_Y + TABLE_TOP + CANDELABRA_H - 0.02f + 0.028f + MID_WAX + 0.03f, TABLE_Z};
    mansion_at(local, out);
}

// A pewter plate at (a, d) on the table, and a goblet beside it, or lying where it rolled.
static void setting(Kit* kit, const KitFrame* f, float a, float d, float side, bool spilt) {
    kit_frame_lathe(kit, f, MAT_STAINLESS, a, d, TABLE_TOP, PLATE, KIT_COUNT(PLATE), 24);
    const float ga = a + 0.17f, gd = d - side * 0.12f;
    if (!spilt) {
        kit_frame_lathe(kit, f, MAT_STAINLESS, ga, gd, TABLE_TOP, GOBLET, KIT_COUNT(GOBLET), 16);
        return;
    }
    const float turn = 0.7f;
    kit_frame_lathe_on(kit, f, MAT_STAINLESS, (vec3){ga, TABLE_TOP + 0.037f, gd},
                       (vec3){cosf(turn), 0.0f, sinf(turn)}, GOBLET, KIT_COUNT(GOBLET), 16);
}

/*
 * The meal nobody came back to: a place laid at every chair, a covered dish in the middle, and a
 * vase of flowers dried on their stems, drooping over its lip.
 */
static void laid(Kit* kit) {
    const KitFrame f = {{TABLE_X, FLOOR_Y, TABLE_Z}, 0.0f};
    setting(kit, &f, -TABLE_HA + 0.23f, 0.0f, 1.0f, false);
    setting(kit, &f, -0.4f, 0.25f, 1.0f, false);
    setting(kit, &f, 0.7f, 0.25f, 1.0f, true);
    setting(kit, &f, -0.4f, -0.25f, -1.0f, false);
    setting(kit, &f, 0.7f, -0.25f, -1.0f, false);

    // The dish and its domed cover.
    const vec2 dish[] = {{0.0f, 0.0f},     {0.12f, 0.0f},   {0.16f, 0.015f}, {0.17f, 0.022f},
                         {0.165f, 0.024f}, {0.12f, 0.012f}, {0.0f, 0.011f}};
    const vec2 cover[] = {{0.13f, 0.0f},   {0.135f, 0.01f}, {0.12f, 0.06f},  {0.08f, 0.1f},
                          {0.03f, 0.115f}, {0.025f, 0.13f}, {0.018f, 0.14f}, {0.0f, 0.142f}};
    kit_frame_lathe(kit, &f, MAT_STAINLESS, -0.55f, 0.0f, TABLE_TOP, dish, KIT_COUNT(dish), 24);
    kit_frame_lathe(kit, &f, MAT_STAINLESS, -0.55f, 0.0f, TABLE_TOP + 0.011f, cover,
                    KIT_COUNT(cover), 20);

    // The vase, and seven stems bowed over, their heads gone to brown.
    const vec2 vase[] = {{0.0f, 0.0f},   {0.04f, 0.0f},   {0.06f, 0.05f},  {0.055f, 0.12f},
                         {0.03f, 0.17f}, {0.035f, 0.19f}, {0.028f, 0.19f}, {0.0f, 0.15f}};
    const float va = 0.55f, vy = TABLE_TOP;
    kit_frame_lathe(kit, &f, MAT_CERAMIC, va, 0.0f, vy, vase, KIT_COUNT(vase), 16);
    const vec2 head[] = {{0.0f, 0.0f}, {0.009f, 0.004f}, {0.011f, 0.011f}, {0.0f, 0.017f}};
    enum { STEMS = 7, POINTS = 8 };
    for (int k = 0; k < STEMS; k++) {
        const float turn = 6.2831853f * (float)k / (float)STEMS + 0.4f;
        const float reach = 0.1f + 0.03f * (float)(k % 3), rise = 0.24f + 0.05f * (float)(k % 2);
        vec3 stem[POINTS] = {{0.0f}};
        for (int i = 0; i < POINTS; i++) {
            const float t = (float)i / (float)(POINTS - 1);
            // Up out of the neck and over: the head hangs lower than the bend.
            const float out = reach * t * t, up = rise * sinf(t * 2.2f) / sinf(2.2f * 0.72f);
            glm_vec3_copy((vec3){va + out * cosf(turn), vy + 0.17f + fmaxf(up, 0.0f) * 0.8f,
                                 out * sinf(turn)},
                          stem[i]);
        }
        kit_frame_pipe(kit, &f, MAT_PRUNE, stem, POINTS, 0.0025f, 5);
        const float* tip = stem[POINTS - 1];
        kit_frame_lathe_on(kit, &f, MAT_PRUNE, tip, (vec3){cosf(turn), -0.8f, sinf(turn)}, head,
                           KIT_COUNT(head), 8);
    }
}

/*
 * The sideboard against the east wall, standing on the panelling's skirting: a plinth, three
 * carved doors with iron rings, and on its top a tureen, a jug, and two chargers stood on edge
 * against the wall.
 */
static void sideboard(Kit* kit) {
    const KitFrame f = {{KITCHEN_X1 - 0.035f, FLOOR_Y, TABLE_Z}, -0.5f * GLM_PIf};
    const float ha = 0.85f, depth = 0.46f, top = 0.92f;
    kit_frame_box(kit, &f, MAT_BLACK, -ha + 0.03f, ha - 0.03f, 0.0f, 0.1f, 0.0f, depth - 0.04f,
                  false);
    kit_frame_box(kit, &f, MAT_MAHOGANY, -ha, ha, 0.1f, top - 0.04f, 0.0f, depth, false);
    kit_frame_box(kit, &f, MAT_MAHOGANY, -ha - 0.03f, ha + 0.03f, top - 0.04f, top, 0.0f,
                  depth + 0.03f, false);
    const float w = 2.0f * ha / 3.0f;
    for (int i = 0; i < 3; i++) {
        const float a0 = -ha + (float)i * w + 0.03f, a1 = a0 + w - 0.06f;
        kit_frame_card_rect(kit, &f, MAT_GOTHIC, GOTHICS[GOTHIC_PANEL_LINENFOLD].uv, a0, a1, 0.16f,
                            top - 0.1f, depth + 0.003f, 1.0f);
        enum { RING = 13 };
        vec3 ring[RING] = {{0.0f}};
        const float ra = i == 2 ? a0 + 0.06f : a1 - 0.06f, ry = 0.58f;
        for (int k = 0; k < RING; k++) {
            const float t = 6.2831853f * (float)k / (float)(RING - 1);
            glm_vec3_copy(
                (vec3){ra + 0.025f * sinf(t), ry - 0.025f - 0.025f * cosf(t), depth + 0.012f},
                ring[k]);
        }
        kit_frame_pipe(kit, &f, MAT_IRON, ring, RING, 0.004f, 6);
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -ha - 0.03f, ha + 0.03f, 0.0f, top, 0.0f,
                  depth + 0.03f, true);

    const vec2 tureen[] = {{0.0f, 0.0f},   {0.06f, 0.0f},   {0.065f, 0.02f}, {0.11f, 0.05f},
                           {0.12f, 0.11f}, {0.11f, 0.14f},  {0.1f, 0.15f},   {0.06f, 0.18f},
                           {0.02f, 0.19f}, {0.025f, 0.21f}, {0.0f, 0.22f}};
    kit_frame_lathe(kit, &f, MAT_STAINLESS, -0.35f, 0.24f, top, tureen, KIT_COUNT(tureen), 20);
    const vec2 jug[] = {{0.0f, 0.0f},    {0.05f, 0.0f},  {0.06f, 0.05f},  {0.055f, 0.12f},
                        {0.035f, 0.18f}, {0.04f, 0.22f}, {0.036f, 0.22f}, {0.0f, 0.2f}};
    kit_frame_lathe(kit, &f, MAT_STAINLESS, 0.45f, 0.22f, top, jug, KIT_COUNT(jug), 16);
    // Leaning back against the wall, each on its rim.
    for (int i = 0; i < 2; i++) {
        const float a = i ? 0.12f : -0.1f;
        kit_frame_lathe_on(kit, &f, MAT_STAINLESS, (vec3){a, top + 0.135f, 0.05f},
                           (vec3){0.0f, 0.25f, 1.0f}, PLATE, KIT_COUNT(PLATE), 24);
    }
}

/*
 * The seat under the window: a carved front, a leather cushion, a board up the wall behind it
 * to the sill, and the sill. The cat watches the rain from here.
 */
static void window_seat(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float cushion = SEAT_TOP - 0.045f;
    kit_frame_box(kit, w, MAT_MAHOGANY, SEAT_X0, SEAT_X1, FLOOR_Y, FLOOR_Y + cushion, KITCHEN_Z0,
                  SEAT_Z1, false);
    kit_frame_card_row(kit, w, MAT_GOTHIC, GOTHICS[GOTHIC_PANEL_LINENFOLD].uv, SEAT_X0 + 0.04f,
                       SEAT_X1 - 0.04f, FLOOR_Y + 0.06f, FLOOR_Y + cushion - 0.04f,
                       SEAT_Z1 + 0.002f, 1.0f, 0.42f);
    kit_frame_box(kit, w, MAT_LEATHER, SEAT_X0 + 0.03f, SEAT_X1 - 0.03f, FLOOR_Y + cushion,
                  FLOOR_Y + SEAT_TOP, KITCHEN_Z0 + 0.03f, SEAT_Z1 - 0.02f, false);
    kit_frame_box(kit, w, MAT_MAHOGANY, SEAT_X0, SEAT_X1, FLOOR_Y + cushion, DINING_WIN_SILL,
                  KITCHEN_Z0, KITCHEN_Z0 + 0.03f, false);
    // Standing a little proud of the opening's foot, which is the wall's own face.
    kit_frame_box(kit, w, MAT_MAHOGANY, KITCHEN_WIN_X0 - 0.06f, KITCHEN_WIN_X1 + 0.06f,
                  DINING_WIN_SILL - 0.01f, DINING_WIN_SILL + 0.02f, HOUSE_FRONT_Z + 0.01f,
                  KITCHEN_Z0 + 0.06f, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, SEAT_X0, SEAT_X1, FLOOR_Y, FLOOR_Y + SEAT_TOP,
                  KITCHEN_Z0, SEAT_Z1, true);
}

// A portrait from gothic.h hung on a wall's face at a along it, its foot at y: a frame's depth
// of backing, and the picture on it.
static void portrait(Kit* kit, const Facade* s, GothicId id, float a, float y) {
    const float w = GOTHICS[id].size[0], h = GOTHICS[id].size[1];
    const float a0 = a - 0.5f * w, a1 = a + 0.5f * w;
    facade_box(kit, s, MAT_MAHOGANY, a0, a1, y, y + h, 0.025f);
    kit_frame_card_rect(kit, &s->f, MAT_GOTHIC, GOTHICS[id].uv, a0, a1, y, y + h,
                        s->face + s->out * 0.026f, s->out);
}

static void portraits(Kit* kit) {
    const Facade east = facade_toward(house_wall(HOUSE_WALL_EAST), TABLE_X, TABLE_Z);
    const Facade north = facade_toward(house_wall(HOUSE_WALL_GREAT_FRONT), TABLE_X, TABLE_Z);
    portrait(kit, &east, GOTHIC_PORTRAIT_A, TABLE_Z, FLOOR_Y + 1.42f);
    portrait(kit, &north, GOTHIC_PORTRAIT_B, TABLE_X, FLOOR_Y + 1.42f);
}

static void rug(Kit* kit) {
    kit_frame_card(kit, &KIT_WORLD, MAT_GOTHIC, (vec3){RUG_X0, FLOOR_Y + 0.008f, RUG_Z0},
                   (vec3){0.0f, 0.0f, RUG_Z1 - RUG_Z0}, (vec3){RUG_X1 - RUG_X0, 0.0f, 0.0f},
                   GOTHICS[GOTHIC_RUG].uv);
}

bool mansion_on_cloth(const vec3 p) {
    if (fabsf(p[1] - FLOOR_Y) < 0.05f && p[0] > RUG_X0 && p[0] < RUG_X1 && p[2] > RUG_Z0 &&
        p[2] < RUG_Z1)
        return true;
    return fabsf(p[1] - (FLOOR_Y + SEAT_TOP)) < 0.05f && p[0] > SEAT_X0 && p[0] < SEAT_X1 &&
           p[2] > KITCHEN_Z0 && p[2] < SEAT_Z1;
}

/*
 * The armour, standing where the player's house has its clock and on the same footing: against
 * the hall's west wall on its panelling's face, opposite the dining room's door, facing into the
 * hall, inside z 12.87..13.43 and 0.3 m short of the probe column down the hall's middle.
 * Polished steel gone dull, its hands on a sword's pommel, the blade's point on its plinth.
 */
static const KitFrame ARMOUR = {
    {HALL_X0 + 0.5f * INT_WALL + PANEL_DEPTH, FLOOR_Y, 0.5f * (KITCHEN_DOOR_Z0 + KITCHEN_DOOR_Z1)},
    0.5f * GLM_PIf};

static void armour(Kit* kit) {
    const KitFrame* f = &ARMOUR;
    const float body = 0.165f; // the body's middle, out from the wall
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.27f, 0.27f, 0.0f, 0.12f, 0.01f, 0.32f, false);

    const vec2 leg[] = {{0.0f, 0.0f},   {0.048f, 0.0f},  {0.055f, 0.1f}, {0.06f, 0.24f},
                        {0.05f, 0.33f}, {0.066f, 0.37f}, {0.07f, 0.41f}, {0.055f, 0.45f},
                        {0.07f, 0.58f}, {0.08f, 0.7f},   {0.0f, 0.72f}};
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s * 0.1f;
        kit_frame_box(kit, f, MAT_STAINLESS, a - 0.045f, a + 0.045f, 0.12f, 0.19f, body - 0.05f,
                      0.31f, false);
        kit_frame_lathe(kit, f, MAT_STAINLESS, a, body, 0.19f, leg, KIT_COUNT(leg), 10);
    }
    const vec2 skirt[] = {{0.0f, 0.0f},   {0.15f, 0.0f}, {0.155f, 0.04f}, {0.145f, 0.05f},
                          {0.15f, 0.09f}, {0.14f, 0.1f}, {0.0f, 0.1f}};
    kit_frame_lathe(kit, f, MAT_STAINLESS, 0.0f, body, 0.84f, skirt, KIT_COUNT(skirt), 10);
    const vec2 cuirass[] = {{0.0f, 0.0f},   {0.13f, 0.0f},   {0.145f, 0.08f},
                            {0.155f, 0.2f}, {0.15f, 0.34f},  {0.12f, 0.43f},
                            {0.07f, 0.46f}, {0.065f, 0.52f}, {0.0f, 0.52f}};
    kit_frame_lathe(kit, f, MAT_STAINLESS, 0.0f, body, 0.92f, cuirass, KIT_COUNT(cuirass), 10);
    const vec2 helm[] = {{0.0f, 0.0f},  {0.09f, 0.0f}, {0.104f, 0.06f}, {0.11f, 0.16f},
                         {0.1f, 0.24f}, {0.07f, 0.3f}, {0.03f, 0.33f},  {0.0f, 0.335f}};
    const float helm_y = 1.44f;
    kit_frame_lathe(kit, f, MAT_STAINLESS, 0.0f, body, helm_y, helm, KIT_COUNT(helm), 12);
    kit_frame_box(kit, f, MAT_BLACK, -0.06f, 0.06f, helm_y + 0.16f, helm_y + 0.175f, body + 0.09f,
                  body + 0.112f, false);
    kit_frame_box(kit, f, MAT_STAINLESS, -0.008f, 0.008f, helm_y + 0.26f, helm_y + 0.36f,
                  body - 0.08f, body + 0.08f, false);

    // The pauldrons, domes over the shoulders, and the arms down to hands on the pommel.
    const vec2 dome[] = {
        {0.0f, 0.0f}, {0.085f, 0.0f}, {0.08f, 0.04f}, {0.055f, 0.07f}, {0.0f, 0.082f}};
    for (int s = -1; s <= 1; s += 2) {
        kit_frame_lathe_on(kit, f, MAT_STAINLESS, (vec3){(float)s * 0.13f, 1.31f, body},
                           (vec3){(float)s, 0.35f, 0.0f}, dome, KIT_COUNT(dome), 10);
        const vec3 arm[] = {{(float)s * 0.19f, 1.3f, body},
                            {(float)s * 0.21f, 1.12f, body + 0.02f},
                            {(float)s * 0.2f, 1.04f, body + 0.06f},
                            {(float)s * 0.07f, 0.97f, 0.27f}};
        kit_frame_pipe(kit, f, MAT_STAINLESS, arm, KIT_COUNT(arm), 0.042f, 8);
    }

    // The sword, point down.
    const float sd = 0.285f;
    kit_frame_box(kit, f, MAT_STAINLESS, -0.022f, 0.022f, 0.12f, 0.86f, sd - 0.006f, sd + 0.006f,
                  false);
    kit_frame_box(kit, f, MAT_STAINLESS, -0.12f, 0.12f, 0.86f, 0.885f, sd - 0.013f, sd + 0.013f,
                  false);
    const vec2 grip[] = {{0.0f, 0.0f},     {0.014f, 0.0f},  {0.013f, 0.11f}, {0.024f, 0.12f},
                         {0.026f, 0.135f}, {0.018f, 0.15f}, {0.0f, 0.152f}};
    kit_frame_lathe(kit, f, MAT_LEATHER, 0.0f, sd, 0.885f, grip, KIT_COUNT(grip), 10);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -0.27f, 0.27f, 0.0f, 1.85f, 0.0f, 0.33f, true);
}

/*
 * The dining room's one light besides the window is the candelabra; the hall's is a floor stand
 * by the armour, clear of the parlour's door and of the probe column.
 */
void mansion_front_build(Kit* kit) {
    table(kit);
    candelabra(kit);
    laid(kit);
    // A chair at either side, one pulled out askew, and one at the west end facing the
    // candelabra, which is the cat's.
    chair(kit, 2.2f, TABLE_Z + TABLE_HD + 0.28f, GLM_PIf);
    chair(kit, 3.3f, TABLE_Z + TABLE_HD + 0.28f, GLM_PIf);
    chair(kit, 2.2f, TABLE_Z - TABLE_HD - 0.28f, 0.0f);
    chair(kit, 3.4f, TABLE_Z - TABLE_HD - 0.42f, -0.35f);
    chair(kit, HEAD_CHAIR_X, TABLE_Z, 0.5f * GLM_PIf);
    sideboard(kit);
    window_seat(kit);
    portraits(kit);
    rug(kit);
    armour(kit);
    candle_stand(kit, &KIT_WORLD, -1.2f, FLOOR_Y, 12.3f, 0.22f);
}
