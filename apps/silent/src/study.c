#include <math.h>

#include "cetra/light.h"

#include "candles.h"
#include "cards.h"
#include "gothic.h"
#include "house.h"
#include "layout.h"
#include "mats.h"
#include "ornament.h"
#include "study.h"

// The study's three plain walls, their faces as the room sees them.
#define EAST_X (HALL_X0 - 0.5f * INT_WALL)
#define WEST_X (HOUSE_X0 + 0.5f * EXT_WALL)

/*
 * A bookcase, in a frame on the floor at a wall's face: a plinth, a back, uprights parting it
 * into bays, SHELVES rows of books in each, a pointed arch over each bay, a frieze of the
 * panelling's quatrefoils, and a cornice.
 */
#define CASE_D      0.34f
#define SHELF_T     0.025f
#define SHELF_PITCH 0.344f
#define SHELVES     5
#define PLINTH      0.1f
#define SPRING      (PLINTH + SHELVES * SHELF_PITCH + 0.02f)
#define HEAD_TOP    2.14f
#define FRIEZE_TOP  (HEAD_TOP + GOTHICS[GOTHIC_FRIEZE].size[1])
#define CASE_TOP    (FRIEZE_TOP + 0.08f)
#define RAIL_Y      2.2f // the ladder's brass rail, under the cornice
#define RAIL_D      (CASE_D + 0.05f)

// The desk, the lamp on it, and their light.
#define DESK_H       0.76f
#define DESK_TOP     (DESK_H + 0.003f) // its leather inset's top, where things stand
#define LAMP_CANDELA 60.0f // a reading lamp's pool: about 940 lux on the desk 0.25 m under it
#define LAMP_RANGE   5.0f
static const float LAMP_COLOUR[3] = {1.0f, 0.74f, 0.45f};

/*
 * A shelf of books from a0 to a1, standing on y under `clear` of headroom: each a leather box
 * whose front is one spine cut whole from a strip, at that spine's own width and the strip's
 * height squashed to the book's, pushed back by a different amount from its neighbours; now and
 * then a gap where one has been taken down.
 */
static void books(Kit* kit, KitRng* rng, const KitFrame* f, float a0, float a1, float y,
                  float clear) {
    float a = a0 + kit_rrange(rng, 0.0f, 0.03f);
    for (;;) {
        if (kit_rnd(rng) < 0.05f) {
            a += kit_rrange(rng, 0.03f, 0.14f);
            continue;
        }
        const GothicStrip* s =
            &GOTHIC_STRIPS[(int)(kit_rnd(rng) * (float)GOTHIC_STRIP_COUNT) % GOTHIC_STRIP_COUNT];
        const int i = (int)(kit_rnd(rng) * (float)s->books) % s->books;
        const float w = s->edges[i + 1] - s->edges[i];
        if (a + w > a1)
            return;
        const GothicSpec* g = &GOTHICS[s->id];
        const float h = fminf(clear - 0.025f, g->size[1] * kit_rrange(rng, 0.78f, 1.0f));
        const float front = CASE_D - kit_rrange(rng, 0.015f, 0.04f);
        const float back = front - kit_rrange(rng, 0.16f, 0.24f);
        // Its front is under the spine, its foot on the shelf and its back to the case.
        kit_frame_box_faces(kit, f, MAT_LEATHER, a, a + w, y, y + h, back, front,
                            KIT_FACE_A_POS | KIT_FACE_A_NEG | KIT_FACE_UP);
        const float du = (g->uv[2] - g->uv[0]) / g->size[0];
        const float slice[4] = {g->uv[0] + du * s->edges[i], g->uv[1],
                                g->uv[0] + du * s->edges[i + 1], g->uv[3]};
        kit_frame_card(kit, f, MAT_GOTHIC, (vec3){a, y, front + 0.001f}, (vec3){w, 0.0f, 0.0f},
                       (vec3){0.0f, h, 0.0f}, slice);
        a += w + kit_rrange(rng, 0.0f, 0.004f);
    }
}

static void bookcase(Kit* kit, KitRng* rng, const KitFrame* f, float a0, float a1, int bays) {
    const float bay = (a1 - a0) / (float)bays;
    kit_frame_box(kit, f, MAT_MAHOGANY, a0, a1, 0.0f, PLINTH, 0.0f, CASE_D - 0.03f, false);
    kit_frame_box(kit, f, MAT_MAHOGANY, a0, a1, PLINTH, CASE_TOP, 0.0f, 0.015f, false);
    for (int b = 0; b <= bays; b++) {
        const float a = a0 + bay * (float)b;
        const float lo = b == 0 ? a : (b == bays ? a - 0.04f : a - 0.015f);
        const float hi = b == 0 ? a + 0.04f : (b == bays ? a : a + 0.015f);
        kit_frame_box(kit, f, MAT_MAHOGANY, lo, hi, 0.0f, HEAD_TOP, 0.0f, CASE_D, false);
    }
    for (int b = 0; b < bays; b++) {
        const float b0 = a0 + bay * (float)b + (b == 0 ? 0.04f : 0.015f);
        const float b1 = a0 + bay * (float)(b + 1) - (b == bays - 1 ? 0.04f : 0.015f);
        for (int s = 0; s <= SHELVES; s++) {
            const float y = PLINTH + SHELF_PITCH * (float)s;
            kit_frame_box(kit, f, MAT_MAHOGANY, b0, b1, y - SHELF_T, y, 0.015f, CASE_D - 0.01f,
                          false);
            if (s < SHELVES)
                books(kit, rng, f, b0, b1, y, SHELF_PITCH - SHELF_T);
        }
        ornament_arch_board(kit, f, MAT_MAHOGANY, b0, b1, SPRING, HEAD_TOP, KIT_ARCH_POINTED,
                            HEAD_TOP - SPRING - 0.02f, CASE_D - 0.03f);
    }

    kit_frame_box(kit, f, MAT_MAHOGANY, a0, a1, HEAD_TOP, FRIEZE_TOP, 0.0f, CASE_D, false);
    kit_frame_card_row(kit, f, MAT_GOTHIC, GOTHICS[GOTHIC_FRIEZE].uv, a0, a1, HEAD_TOP, FRIEZE_TOP,
                       CASE_D + 0.001f, 1.0f, GOTHICS[GOTHIC_FRIEZE].size[0]);
    const vec2 cornice[] = {{0.0f, FRIEZE_TOP},
                            {CASE_D, FRIEZE_TOP},
                            {CASE_D + 0.03f, FRIEZE_TOP + 0.015f},
                            {CASE_D + 0.06f, FRIEZE_TOP + 0.05f},
                            {CASE_D + 0.06f, CASE_TOP},
                            {0.0f, CASE_TOP}};
    kit_frame_run(kit, f, MAT_MAHOGANY, cornice, KIT_COUNT(cornice), a0, a1);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a0, a1, 0.0f, CASE_TOP, 0.0f, CASE_D + 0.02f, true);
}

/*
 * The library ladder, leaning on the long case's brass rail at `a`: two stiles from its foot out
 * on the floor to the rail, rungs between them, and the rail itself on brackets along the case.
 */
static void ladder(Kit* kit, const KitFrame* f, float a, float a0, float a1) {
    kit_frame_bar(kit, f, MAT_BRASS, a0 + 0.05f, a1 - 0.05f, RAIL_Y, RAIL_D, 0.012f);
    for (float b = a0 + 0.3f; b < a1 - 0.2f; b += 1.0f)
        kit_frame_box(kit, f, MAT_BRASS, b - 0.012f, b + 0.012f, RAIL_Y - 0.012f, RAIL_Y + 0.012f,
                      CASE_D, RAIL_D, false);
    const float foot = 0.8f, top = RAIL_Y + 0.05f, lean = RAIL_D + 0.03f;
    for (int s = -1; s <= 1; s += 2) {
        const float c = a + 0.22f * (float)s;
        const vec2 stile[4] = {
            {foot, 0.0f}, {foot + 0.05f, 0.0f}, {lean + 0.05f, top}, {lean, top}};
        kit_frame_run(kit, f, MAT_MAHOGANY, stile, 4, c - 0.02f, c + 0.02f);
    }
    for (float y = 0.28f; y < top - 0.15f; y += 0.28f) {
        const float d = foot + 0.025f + (lean - foot) * y / top;
        kit_frame_bar(kit, f, MAT_MAHOGANY, a - 0.2f, a + 0.2f, y, d, 0.014f);
    }
}

// A brass knob turned out of a face along `axis`.
static void knob(Kit* kit, const KitFrame* f, float a, float y, float d, const vec3 axis) {
    const vec2 profile[] = {{0.0f, 0.0f},     {0.011f, 0.0f},   {0.011f, 0.005f},
                            {0.007f, 0.011f}, {0.013f, 0.021f}, {0.0f, 0.025f}};
    kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){a, y, d}, axis, profile, KIT_COUNT(profile), 10);
}

/*
 * The desk, in a frame whose +d is the way its sitter faces: a top with a leather inset, two
 * pedestals of three drawers each and a drawer over the knee-hole, all facing the sitter, and
 * linenfold carved across its back and down its pedestals' outer sides, where the room sees it.
 */
static void desk(Kit* kit, const KitFrame* f) {
    const float top = DESK_H, under = top - 0.045f, front = -0.36f, face = -0.372f;
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.8f, 0.8f, under, top, -0.4f, 0.4f, false);
    kit_frame_box(kit, f, MAT_LEATHER, -0.66f, 0.66f, top, DESK_TOP, -0.3f, 0.28f, false);
    const vec3 toward = {0.0f, 0.0f, -1.0f};
    const GothicSpec* fold = &GOTHICS[GOTHIC_PANEL_LINENFOLD];
    for (int s = -1; s <= 1; s += 2) {
        const float sf = (float)s, lo = fminf(0.3f * sf, 0.76f * sf),
                    hi = fmaxf(0.3f * sf, 0.76f * sf);
        kit_frame_box(kit, f, MAT_MAHOGANY, lo, hi, 0.06f, under, front, 0.36f, false);
        kit_frame_box(kit, f, MAT_MAHOGANY, lo - 0.015f, hi + 0.015f, 0.0f, 0.06f, -0.375f, 0.375f,
                      false);
        for (int k = 0; k < 3; k++) {
            const float y0 = 0.1f + 0.2f * (float)k, y1 = y0 + 0.17f;
            kit_frame_box(kit, f, MAT_MAHOGANY, lo + 0.025f, hi - 0.025f, y0, y1, face, front,
                          false);
            knob(kit, f, 0.5f * (lo + hi), 0.5f * (y0 + y1), face, toward);
        }
        // The outer side, carved, facing out along a.
        const float side = 0.761f * sf;
        kit_frame_card(kit, f, MAT_GOTHIC, (vec3){side, 0.08f, 0.33f * sf},
                       (vec3){0.0f, 0.0f, -0.66f * sf}, (vec3){0.0f, under - 0.12f, 0.0f},
                       fold->uv);
    }
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.3f, 0.3f, top - 0.15f, under, front, 0.36f, false);
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.28f, 0.28f, top - 0.14f, under - 0.01f, face, front,
                  false);
    knob(kit, f, 0.0f, top - 0.1f, face, toward);
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.3f, 0.3f, 0.1f, top - 0.15f, 0.33f, 0.36f, false);
    kit_frame_card_row(kit, f, MAT_GOTHIC, fold->uv, -0.76f, 0.76f, 0.08f, under - 0.04f, 0.361f,
                       1.0f, fold->size[0]);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -0.8f, 0.8f, 0.0f, top, -0.4f, 0.4f, true);
}

/*
 * The desk's chair, in a frame facing it: four legs and stretchers, a seat with a leather
 * cushion, and a tall back shaped as a lancet, its front carved with blind tracery, a small
 * finial over each shoulder.
 */
static void chair(Kit* kit, const KitFrame* f) {
    const float seat = 0.46f, back0 = -0.25f, back1 = -0.21f;
    for (int i = 0; i < 4; i++) {
        const float a = i & 1 ? 0.2f : -0.2f, d = i & 2 ? 0.2f : -0.2f;
        kit_frame_box(kit, f, MAT_MAHOGANY, a - 0.022f, a + 0.022f, 0.0f, seat - 0.05f, d - 0.022f,
                      d + 0.022f, false);
    }
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.2f, 0.2f, 0.12f, 0.15f, -0.21f, -0.19f, false);
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.2f, 0.2f, 0.12f, 0.15f, 0.19f, 0.21f, false);
    kit_frame_box(kit, f, MAT_MAHOGANY, -0.25f, 0.25f, seat - 0.05f, seat, -0.24f, 0.24f, false);
    kit_frame_box(kit, f, MAT_LEATHER, -0.22f, 0.22f, seat, seat + 0.045f, -0.2f, 0.22f, false);

    const KitOpening shape = {-0.25f, 0.25f, seat, 1.12f, KIT_ARCH_POINTED, 0.32f};
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(&shape, outline);
    kit_frame_extrude(kit, f, MAT_MAHOGANY, outline, n, back0, back1);
    const GothicSpec* tracery = &GOTHICS[GOTHIC_PANEL_TRACERY];
    kit_frame_card(kit, f, MAT_GOTHIC, (vec3){-0.21f, seat + 0.06f, back1 + 0.001f},
                   (vec3){0.42f, 0.0f, 0.0f}, (vec3){0.0f, 1.1f - seat - 0.06f, 0.0f}, tracery->uv);
    const vec2 finial[] = {{0.0f, 0.0f},    {0.022f, 0.0f}, {0.022f, 0.02f}, {0.012f, 0.035f},
                           {0.026f, 0.07f}, {0.01f, 0.1f},  {0.006f, 0.15f}, {0.0f, 0.16f}};
    for (int s = -1; s <= 1; s += 2)
        kit_frame_lathe(kit, f, MAT_MAHOGANY, 0.23f * (float)s, 0.5f * (back0 + back1), 1.12f,
                        finial, KIT_COUNT(finial), 8);
    // Solid up to the cushion's top, and the back to its full height; the space over the
    // cushion is left open, since the house's cat sleeps there and jumps up and down from it.
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -0.27f, 0.27f, 0.0f, seat + 0.045f, -0.26f, 0.25f,
                  true);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -0.27f, 0.27f, seat, 1.0f, -0.26f, back1, true);
}

/*
 * The lamp: a turned brass column and an arm out over the desk, under it a green cased-glass
 * shade glowing a little, and inside its rim the diffuser that lights the desk -- a downward
 * spot, without shadows, which the shade's own cut-off makes wide.
 */
static void lamp(Kit* kit, Scene* scene, const KitFrame* f, float a, float d) {
    const float y = DESK_TOP;
    const vec2 column[] = {{0.0f, 0.0f},     {0.075f, 0.0f},  {0.075f, 0.012f}, {0.05f, 0.03f},
                           {0.016f, 0.045f}, {0.012f, 0.05f}, {0.009f, 0.33f},  {0.016f, 0.34f},
                           {0.016f, 0.36f},  {0.0f, 0.36f}};
    kit_frame_lathe(kit, f, MAT_BRASS, a, d, y, column, KIT_COUNT(column), 16);
    const float top = y + 0.35f, at = a + 0.2f, rim = top - 0.12f;
    kit_frame_pipe(kit, f, MAT_BRASS, (vec3[]){{a, top, d}, {at, top, d}}, 2, 0.008f, 8);
    const vec2 shade[] = {{0.13f, 0.0f},  {0.125f, 0.02f}, {0.1f, 0.06f},
                          {0.055f, 0.1f}, {0.02f, 0.118f}, {0.0f, 0.12f}};
    kit_frame_lathe(kit, f, MAT_SHADE, at, d, rim, shade, KIT_COUNT(shade), 20);
    kit_frame_prism(kit, f, MAT_BULB, at, d, rim + 0.01f, rim + 0.014f, 0.12f, 16);

    vec3 pos = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, at, rim + 0.02f, d, pos);
    glm_vec3_add(pos, kit->origin, pos); // a light is no part of the kit, so it moves itself
    LightDesc desc = {.name = "study_lamp",
                      .type = LIGHT_SPOT,
                      .position = {pos[0], pos[1], pos[2]},
                      .direction = {0.0f, -1.0f, 0.0f},
                      .color = {LAMP_COLOUR[0], LAMP_COLOUR[1], LAMP_COLOUR[2]},
                      .intensity = LAMP_CANDELA,
                      .range = LAMP_RANGE,
                      .inner_cutoff = glm_rad(40.0f),
                      .outer_cutoff = glm_rad(62.0f)};
    scene_add_light(scene, create_light(&desc));
}

// A card lying face up on the desk at (a, d), turned `yaw`, read from the sitter's side.
static void lying(Kit* kit, const KitFrame* f, CardId id, float a, float d, float lift, float yaw,
                  float scale) {
    // The sitter is at -d, so the card's top is turned half round from the kit's default.
    kit_frame_card_lying(kit, f, MAT_CARDS, CARDS[id].uv, scale * CARDS[id].size[0],
                         scale * CARDS[id].size[1], a, DESK_TOP + lift, d, GLM_PIf - yaw);
}

// The journal lying open: leather boards, two page blocks, and a written page on each.
static void journal(Kit* kit, const KitFrame* f, float a, float d) {
    const float y = DESK_TOP;
    kit_frame_box(kit, f, MAT_LEATHER, a - 0.18f, a + 0.18f, y, y + 0.005f, d - 0.125f, d + 0.125f,
                  false);
    const float pages = 0.022f; // the page blocks' top, over the boards' foot
    for (int s = -1; s <= 1; s += 2) {
        kit_frame_box(kit, f, MAT_PAPER, a + 0.004f * (float)s, a + 0.17f * (float)s, y + 0.005f,
                      y + pages, d - 0.117f, d + 0.117f, false);
        lying(kit, f, CARD_LETTER, a + 0.087f * (float)s, d, pages + 0.0005f, 0.0f, 0.76f);
    }
}

/*
 * What is on the desk: the lamp at the sitter's right, a pair of candlesticks at the left, the
 * journal open in front of the chair, two letters beside it, and an inkwell with a pen in it.
 */
static void desk_things(Kit* kit, Scene* scene, const KitFrame* f) {
    lamp(kit, scene, f, -0.66f, 0.2f);
    candle_stick(kit, f, 0.62f, DESK_TOP, 0.2f, 0.14f);
    candle_stick(kit, f, 0.48f, DESK_TOP, 0.26f, 0.09f);
    journal(kit, f, 0.0f, -0.12f);
    lying(kit, f, CARD_LETTER, 0.38f, -0.1f, 0.001f, 0.35f, 1.0f);
    lying(kit, f, CARD_NOTE, 0.3f, 0.06f, 0.002f, -0.2f, 1.0f);
    const float y = DESK_TOP;
    const vec2 well[] = {{0.0f, 0.0f},    {0.032f, 0.0f},   {0.032f, 0.035f}, {0.02f, 0.045f},
                         {0.012f, 0.05f}, {0.012f, 0.058f}, {0.0f, 0.058f}};
    kit_frame_lathe(kit, f, MAT_BLACK, -0.26f, 0.04f, y, well, KIT_COUNT(well), 12);
    kit_frame_pipe(kit, f, MAT_PAPER,
                   (vec3[]){{-0.26f, y + 0.03f, 0.04f}, {-0.32f, y + 0.22f, 0.09f}}, 2, 0.003f, 6);
}

/*
 * A library globe: three legs splayed up from the floor to a mahogany horizon ring, a turned
 * column, the parchment-coloured globe on its tilted axis, and the brass meridian round it.
 */
static void globe(Kit* kit, float x, float z) {
    const KitFrame f = {{x, FLOOR2_Y, z}, 0.6f};
    const float ring_y = 0.78f, ring_r = 0.3f, r = 0.24f, cy = 0.86f;
    for (int k = 0; k < 3; k++) {
        const float t = 2.0f * GLM_PIf * (float)k / 3.0f, c = cosf(t), s = sinf(t);
        kit_frame_pipe(kit, &f, MAT_MAHOGANY,
                       (vec3[]){{0.36f * c, 0.0f, 0.36f * s},
                                {0.31f * c, 0.4f, 0.31f * s},
                                {ring_r * c, ring_y, ring_r * s}},
                       3, 0.018f, 8);
    }
    const vec2 column[] = {{0.0f, 0.0f},   {0.05f, 0.0f},  {0.05f, 0.04f}, {0.025f, 0.08f},
                           {0.04f, 0.2f},  {0.02f, 0.35f}, {0.03f, 0.45f}, {0.018f, 0.55f},
                           {0.026f, 0.6f}, {0.0f, 0.6f}};
    kit_frame_lathe(kit, &f, MAT_MAHOGANY, 0.0f, 0.0f, 0.0f, column, KIT_COUNT(column), 10);

    // The meridian stands upright in the column's slot, and the axis tilts inside it.
    enum { LOOP = 25, ARC = 14 };
    vec3 ring[LOOP] = {{0.0f}}, meridian[LOOP] = {{0.0f}};
    const float tilt = glm_rad(23.4f), mr = r + 0.02f;
    for (int i = 0; i < LOOP; i++) {
        const float t = 2.0f * GLM_PIf * (float)i / (float)(LOOP - 1), c = cosf(t), s = sinf(t);
        glm_vec3_copy((vec3){ring_r * c, ring_y, ring_r * s}, ring[i]);
        glm_vec3_copy((vec3){mr * c, cy + mr * s, 0.0f}, meridian[i]);
    }
    kit_frame_pipe(kit, &f, MAT_MAHOGANY, ring, LOOP, 0.02f, 6);
    kit_frame_pipe(kit, &f, MAT_BRASS, meridian, LOOP, 0.007f, 6);
    kit_frame_pipe(kit, &f, MAT_BRASS,
                   (vec3[]){{-mr * sinf(tilt), cy - mr * cosf(tilt), 0.0f},
                            {mr * sinf(tilt), cy + mr * cosf(tilt), 0.0f}},
                   2, 0.005f, 6);
    vec2 sphere[ARC + 1];
    for (int i = 0; i <= ARC; i++) {
        const float t = GLM_PIf * (float)i / (float)ARC;
        glm_vec2_copy((vec2){r * sinf(t), r - r * cosf(t)}, sphere[i]);
    }
    kit_frame_lathe(kit, &f, MAT_PAPER, 0.0f, 0.0f, cy - r, sphere, ARC + 1, 24);
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -0.36f, 0.36f, 0.0f, cy + r, -0.36f, 0.36f, true);
}

void study_build(Kit* kit, Scene* scene, unsigned int seed) {
    KitRng rng = {seed * 2654435761u + 977u};

    // Along the east wall from the front corner to the back: four bays, and the ladder.
    const KitFrame east = {{EAST_X, FLOOR2_Y, BAND_Z0}, -0.5f * GLM_PIf};
    const float east_len = BAND_Z1 - BAND_Z0;
    bookcase(kit, &rng, &east, 0.0f, east_len, 4);
    ladder(kit, &east, 2.4f, 0.0f, east_len);

    // The doorway's casing on this side, and along the back from the west corner to 4 cm short
    // of it, and down the west wall from in front of that one to the tower.
    const KitWall* front = house_wall(HOUSE_WALL_GREAT_FRONT);
    const KitOpening* door = &front->openings[OPENING_STUDY_DOOR];
    const Facade inside = facade_inner(front);
    ornament_casing(kit, &inside, MAT_MAHOGANY, door);
    const float door_x = door->from - ORNAMENT_CASING_W - 0.04f;
    const KitFrame back = {{door_x, FLOOR2_Y, BAND_Z1}, GLM_PIf};
    bookcase(kit, &rng, &back, 0.0f, door_x - WEST_X, 2);
    const float west_from = BAND_Z1 - CASE_D, west_to = TOWER_Z + TOWER_APOTHEM + 0.05f;
    const KitFrame west = {{WEST_X, FLOOR2_Y, west_from}, 0.5f * GLM_PIf};
    bookcase(kit, &rng, &west, 0.0f, west_from - west_to, 1);

    // In the bay, the desk facing into the room with the stained glass behind its chair.
    const KitFrame at_desk = {{-5.0f, FLOOR2_Y, 9.9f}, 0.0f};
    desk(kit, &at_desk);
    desk_things(kit, scene, &at_desk);
    const KitFrame at_chair = {{-5.02f, FLOOR2_Y, 9.0f}, 0.12f};
    chair(kit, &at_chair);
    globe(kit, -6.4f, 10.9f);
}
