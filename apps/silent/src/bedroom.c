#include <math.h>

#include "cetra/light.h"

#include "bedroom.h"
#include "cards.h"
#include "home.h"
#include "mats.h"
#include "ornament.h"

/*
 * The bed stands out from the east wall under its window, so its frame is the wall's: a along
 * +z, d out into the room along -x. Its headboard stays under the window's sill. The room is
 * 2.76 m front to back, too short for a bed and a dresser at its foot, so the bed runs across it
 * and the dresser stands on the hall wall facing it, between the door and the front wall: the
 * door hangs on its back jamb and stands open along the stretch behind.
 */
#define BED_Z      (0.5f * (BEDROOM_IN_Z0 + BEDROOM_IN_Z1))
#define BED_HALF   0.75f // the frame's half width, posts included
#define BED_LONG   2.0f  // from the wall to the footboard's outer face
#define MATTRESS_Y 0.33f // the mattress's underside, on the slats
#define MATTRESS_T 0.22f
#define QUILT_T    0.06f
#define HEAD_TOP   (GROUND_SILL - FLOOR_Y - 0.03f)
#define FOOT_TOP   0.5f

// The nightstands' half width along the wall, depth and height, either side of the bed.
#define STAND_HALF  0.23f
#define STAND_DEPTH 0.4f
#define STAND_H     0.58f

// The chest of drawers, from the front wall to the door's casing.
#define CHEST_Z0    (BEDROOM_IN_Z0 + 0.03f)
#define CHEST_Z1    (BEDROOM_DOOR_Z0 - ORNAMENT_CASING_W - 0.03f)
#define CHEST_DEPTH 0.46f
#define CHEST_H     1.05f

#define LAMP_CANDELA 40.0f // the living room's floor lamp's

// What stands against a wall stands off it past the paper and the skirting.
#define OFF_WALL (LINING + 0.02f)

static const KitFrame BED = {{BEDROOM_IN_X1 - OFF_WALL, FLOOR_Y, BED_Z}, -0.5f * GLM_PIf};

void bedroom_bed_top(vec3 out) {
    kit_frame_point(&BED, 0.0f, MATTRESS_Y + MATTRESS_T + QUILT_T, 1.45f, out);
}

// A brass knob turned out of a face along +d.
static void knob(Kit* kit, const KitFrame* f, float a, float y, float d) {
    const vec2 profile[] = {{0.0f, 0.0f},     {0.010f, 0.0f},   {0.010f, 0.005f},
                            {0.006f, 0.010f}, {0.012f, 0.019f}, {0.0f, 0.023f}};
    kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){a, y, d}, (vec3){0.0f, 0.0f, 1.0f}, profile,
                       KIT_COUNT(profile), 10);
}

// A square post from the floor to `top` at (a, d), capped with a ball.
static void post(Kit* kit, const KitFrame* f, float a, float d, float top) {
    const float h = 0.03f;
    kit_frame_box(kit, f, MAT_CASE, a - h, a + h, 0.0f, top, d - h, d + h, false);
    const vec2 ball[] = {{0.0f, 0.0f},    {0.022f, 0.004f}, {0.026f, 0.02f},
                         {0.02f, 0.036f}, {0.006f, 0.044f}, {0.0f, 0.045f}};
    kit_frame_lathe(kit, f, MAT_CASE, a, d, top, ball, KIT_COUNT(ball), 12);
}

/*
 * The bed: a frame of posts, a panelled head- and footboard and side rails, a mattress, a quilt
 * turned back off the pillows, its sides hanging over the rails.
 */
static void bed(Kit* kit) {
    const KitFrame* f = &BED;
    const float in = BED_HALF - 0.03f;
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s * in;
        post(kit, f, a, 0.03f, HEAD_TOP - 0.045f);
        post(kit, f, a, BED_LONG - 0.03f, FOOT_TOP);
        kit_frame_box(kit, f, MAT_CASE, a - 0.015f, a + 0.015f, 0.16f, 0.32f, 0.06f,
                      BED_LONG - 0.06f, false);
    }
    // The boards between the posts: a rail top and bottom and a panel between.
    const float ends[2][2] = {{0.01f, HEAD_TOP - 0.06f}, {BED_LONG - 0.05f, FOOT_TOP - 0.02f}};
    for (int e = 0; e < 2; e++) {
        const float d = ends[e][0], top = ends[e][1];
        kit_frame_box(kit, f, MAT_CASE, -in, in, top - 0.07f, top, d, d + 0.04f, false);
        kit_frame_box(kit, f, MAT_CASE, -in, in, 0.16f, 0.24f, d, d + 0.04f, false);
        kit_frame_box(kit, f, MAT_CASE, -in + 0.03f, in - 0.03f, 0.24f, top - 0.07f, d + 0.01f,
                      d + 0.03f, false);
    }
    kit_frame_box(kit, f, MAT_CASE, -in, in, MATTRESS_Y - 0.03f, MATTRESS_Y, 0.06f,
                  BED_LONG - 0.06f, false);

    // The mattress, made up in the sheet; the pillows against the headboard.
    const float m = in - 0.03f, top = MATTRESS_Y + MATTRESS_T;
    kit_frame_soft_box(kit, f, MAT_BEDDING, -m, m, MATTRESS_Y, top, 0.06f, BED_LONG - 0.06f, 0.06f,
                       0.008f);
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s * 0.33f;
        kit_frame_soft_box(kit, f, MAT_BEDDING, a - 0.31f, a + 0.31f, top - 0.02f, top + 0.13f,
                           0.09f, 0.5f, 0.06f, 0.03f);
    }
    // The quilt from below the pillows to the foot, hanging over the sides, turned back at its
    // head in a roll that shows its lining.
    const float q0 = 0.62f, q1 = BED_LONG - 0.07f, over = in + 0.04f;
    kit_frame_soft_box(kit, f, MAT_QUILT, -over, over, top - 0.01f, top + QUILT_T, q0, q1, 0.03f,
                       0.012f);
    for (int s = -1; s <= 1; s += 2) {
        const float a0 = (float)s * (in + 0.005f), a1 = (float)s * over;
        kit_frame_soft_box(kit, f, MAT_QUILT, a0, a1, 0.22f, top + QUILT_T - 0.02f, q0, q1, 0.015f,
                           0.0f);
    }
    kit_frame_soft_box(kit, f, MAT_QUILT, -over + 0.02f, over - 0.02f, top + 0.02f,
                       top + QUILT_T + 0.06f, q0 - 0.08f, q0 + 0.1f, 0.05f, 0.01f);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -BED_HALF, BED_HALF, 0.0f, top + QUILT_T, 0.0f,
                  BED_LONG, true);
}

/*
 * A nightstand at `a` along the bed's wall: a carcass on short legs with a drawer under its top
 * and an open shelf below, its top overhanging a little.
 */
static void nightstand(Kit* kit, float a) {
    const KitFrame* f = &BED;
    const float h = STAND_HALF, d1 = STAND_DEPTH;
    for (int i = 0; i < 4; i++) {
        const float la = a + ((i & 1) ? h - 0.03f : -h + 0.03f), ld = (i & 2) ? d1 - 0.03f : 0.03f;
        kit_frame_box(kit, f, MAT_CASE, la - 0.02f, la + 0.02f, 0.0f, 0.1f, ld - 0.02f, ld + 0.02f,
                      false);
    }
    kit_frame_box(kit, f, MAT_CASE, a - h, a + h, 0.1f, 0.12f, 0.0f, d1, false);
    kit_frame_box(kit, f, MAT_CASE, a - h, a - h + 0.02f, 0.12f, STAND_H - 0.03f, 0.0f, d1, false);
    kit_frame_box(kit, f, MAT_CASE, a + h - 0.02f, a + h, 0.12f, STAND_H - 0.03f, 0.0f, d1, false);
    kit_frame_box(kit, f, MAT_CASE, a - h, a + h, 0.12f, STAND_H - 0.03f, 0.0f, 0.02f, false);
    kit_frame_box(kit, f, MAT_CASE, a - h + 0.02f, a + h - 0.02f, 0.3f, 0.32f, 0.02f, d1, false);
    kit_frame_box(kit, f, MAT_CASE, a - h - 0.015f, a + h + 0.015f, STAND_H - 0.03f, STAND_H, 0.0f,
                  d1 + 0.015f, false);
    // The drawer, and the dark of the shelf behind the opening.
    const float y0 = STAND_H - 0.2f, y1 = STAND_H - 0.045f;
    kit_frame_box(kit, f, MAT_CASE, a - h + 0.025f, a + h - 0.025f, y0, y1, d1 - 0.02f, d1 + 0.005f,
                  false);
    kit_frame_box(kit, f, MAT_CASE, a - h + 0.02f, a + h - 0.02f, y0 - 0.015f, y0, 0.02f, d1,
                  false);
    knob(kit, f, a, 0.5f * (y0 + y1), d1 + 0.005f);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - h, a + h, 0.0f, STAND_H, 0.0f, d1, true);
}

/*
 * The lamp on the front nightstand: a ceramic urn of a base and a drum shade, lit from inside,
 * which is the room's one light. Its light is cached, its body a bulb's. It is a kit of its own
 * that casts nothing: a fabric shade lets its light through, and drawn into the lamp's own
 * shadow it boxed the bulb in, lighting the wall above it and leaving the room dark.
 */
static void lamp(Engine* engine, Scene* scene, float a) {
    Kit kit;
    kit_init(&kit, scene, NULL, NULL);
    mats_register(&kit, engine, scene);
    kit.casts_nothing = true;
    const KitFrame* f = &BED;
    const float d = 0.5f * STAND_DEPTH, y = STAND_H;
    const vec2 base[] = {{0.0f, 0.0f},   {0.06f, 0.0f}, {0.06f, 0.012f}, {0.045f, 0.03f},
                         {0.07f, 0.11f}, {0.06f, 0.2f}, {0.02f, 0.24f},  {0.015f, 0.29f},
                         {0.008f, 0.3f}, {0.0f, 0.3f}};
    kit_frame_lathe(&kit, f, MAT_CERAMIC, a, d, y, base, KIT_COUNT(base), 16);
    const float shade_y = y + 0.26f;
    const vec2 shade[] = {
        {0.16f, 0.0f}, {0.155f, 0.004f}, {0.1f, 0.2f}, {0.095f, 0.2f}, {0.15f, 0.004f}};
    kit_frame_lathe(&kit, f, MAT_LAMPSHADE, a, d, shade_y, shade, KIT_COUNT(shade), 18);
    kit_finish(&kit, "bedside_lamp");
    vec3 at = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a, shade_y + 0.09f, d, at);
    LightDesc light = {.name = "bedside_lamp",
                       .type = LIGHT_POINT,
                       .position = {at[0], at[1], at[2]},
                       .color = {1.0f, 0.7f, 0.42f},
                       .intensity = LAMP_CANDELA,
                       .range = 5.0f,
                       .cast_shadows = true,
                       .shadow_cache = true,
                       .source_radius = 0.03f,
                       .shadow_near = 0.05f};
    scene_add_light(scene, create_light(&light));
}

// On the other nightstand: a book left face down and a glass of water gone stale.
static void bedside_things(Kit* kit, float a) {
    const KitFrame* f = &BED;
    KitFrame book = {{0.0f, 0.0f, 0.0f}, f->yaw + 0.35f};
    kit_frame_point(f, a + 0.04f, STAND_H, 0.17f, book.origin);
    kit_frame_box(kit, &book, MAT_LEATHER, -0.08f, 0.08f, 0.0f, 0.028f, -0.11f, 0.11f, false);
    kit_frame_box(kit, &book, MAT_PAPER, -0.075f, 0.075f, 0.003f, 0.025f, -0.105f, 0.105f, false);
    const vec2 glass[] = {{0.0f, 0.0f},    {0.03f, 0.0f},    {0.034f, 0.11f},
                          {0.031f, 0.11f}, {0.028f, 0.008f}, {0.0f, 0.008f}};
    kit_frame_lathe(kit, f, MAT_GLASS_CLEAR, a - 0.12f, 0.27f, STAND_H, glass, KIT_COUNT(glass),
                    14);
}

/*
 * The chest of drawers on the hall wall, facing the bed: a plinth, four graduated drawers with
 * two knobs each, an overhanging top, and on it a photograph in a frame on its stand.
 */
static void chest(Kit* kit) {
    const KitFrame f = {{BEDROOM_IN_X0 + OFF_WALL, FLOOR_Y, 0.5f * (CHEST_Z0 + CHEST_Z1)},
                        0.5f * GLM_PIf};
    const float h = 0.5f * (CHEST_Z1 - CHEST_Z0), d1 = CHEST_DEPTH, top = CHEST_H - 0.03f;
    kit_frame_box(kit, &f, MAT_CASE, -h + 0.02f, h - 0.02f, 0.0f, 0.08f, 0.0f, d1 - 0.02f, false);
    kit_frame_box(kit, &f, MAT_CASE, -h, h, 0.08f, top, 0.0f, d1, false);
    kit_frame_box(kit, &f, MAT_CASE, -h - 0.02f, h + 0.02f, top, CHEST_H, 0.0f, d1 + 0.02f, false);
    const float heights[4] = {0.24f, 0.22f, 0.2f, 0.18f};
    float y = 0.1f;
    for (int i = 0; i < 4; i++) {
        const float y0 = y, y1 = y + heights[i] - 0.015f;
        kit_frame_box(kit, &f, MAT_CASE, -h + 0.025f, h - 0.025f, y0, y1, d1, d1 + 0.02f, false);
        for (int s = -1; s <= 1; s += 2)
            knob(kit, &f, (float)s * 0.45f * h, 0.5f * (y0 + y1), d1 + 0.02f);
        y += heights[i];
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -h - 0.02f, h + 0.02f, 0.0f, CHEST_H, 0.0f,
                  d1 + 0.04f, true);

    // The photograph: a black frame leaning back on its stand, the picture on its face.
    const CardSpec* photo = &CARDS[CARD_PHOTO_LAKE];
    const float w = photo->size[0] + 0.03f, ph = photo->size[1] + 0.03f, lean = 0.2f;
    KitFrame stand = {{0.0f, 0.0f, 0.0f}, f.yaw};
    kit_frame_point(&f, 0.12f, CHEST_H, 0.18f, stand.origin);
    const vec3 base = {-0.5f * w, 0.0f, 0.0f}, across = {w, 0.0f, 0.0f};
    const vec3 up = {0.0f, ph * cosf(lean), -ph * sinf(lean)};
    kit_frame_card(kit, &stand, MAT_BLACK, base, across, up, (float[4]){0.0f, 0.0f, 1.0f, 1.0f});
    const vec3 inset = {-0.5f * photo->size[0], 0.015f * cosf(lean), 0.004f - 0.015f * sinf(lean)};
    const vec3 p_up = {0.0f, photo->size[1] * cosf(lean), -photo->size[1] * sinf(lean)};
    kit_frame_card(kit, &stand, MAT_CARDS, inset, (vec3){photo->size[0], 0.0f, 0.0f}, p_up,
                   photo->uv);
}

void bedroom_build(Kit* kit, Engine* engine, Scene* scene) {
    bed(kit);
    const float front = BEDROOM_IN_Z0 - BED_Z + STAND_HALF + 0.05f;
    const float back = BEDROOM_IN_Z1 - BED_Z - STAND_HALF - 0.05f;
    nightstand(kit, front);
    nightstand(kit, back);
    lamp(engine, scene, front);
    bedside_things(kit, back);
    chest(kit);
    // A rag rug on the boards between the bed and the chest.
    kit_frame_box(kit, &KIT_WORLD, MAT_RUG, 1.05f, 2.75f, FLOOR_Y, FLOOR_Y + 0.008f, BED_Z - 0.75f,
                  BED_Z + 0.75f, false);
}
