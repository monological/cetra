#include <math.h>
#include <stdio.h>

#include "cards.h"
#include "clock.h"
#include "layout.h"
#include "mats.h"

/*
 * Against the hall's west wall, standing on the panelling's face, opposite the
 * kitchen door, facing into the hall. The frame's a runs along the wall and d
 * out of it, so the case is drawn as if it stood against any wall. It is kept
 * inside z 12.87..13.43, and its front 0.3 m short of the GI probe column down
 * the hall's middle at x -0.795: a probe inside solid geometry darkens
 * everything it lights.
 */
static const KitFrame CLOCK = {
    {HALL_X0 + 0.5f * INT_WALL + PANEL_DEPTH, FLOOR_Y, 0.5f * (KITCHEN_DOOR_Z0 + KITCHEN_DOOR_Z1)},
    0.5f * GLM_PIf};

#define COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

// Heights above the floor, the case's own and its dial's.
#define WAIST_Y0 0.56f
#define WAIST_Y1 1.50f
#define HOOD_Y0  1.56f
#define HOOD_Y1  2.12f
#define DIAL_Y0  1.60f   // the dial card's foot, inside the hood door's lower rail
#define DIAL_D   0.2508f // the painted dial's face, a hair in front of its board

// The movement: a seconds pendulum, one beat each way, hung from a pivot
// behind the dial, and hands that read 3:07 when the house is first seen.
#define PERIOD      2.0
#define SWING       0.07f // radians of swing each side: about four degrees
#define PIVOT_Y     1.64f
#define PIVOT_D     0.14f
#define PENDULUM_L  0.97f   // pivot to the bob's middle
#define BOB_D       -0.012f // the bob's middle, off the rod along the case's depth
#define START_TIME  (3.0 * 3600.0 + 7.0 * 60.0)
#define BEAT_VOLUME 0.5f

/*
 * A turned column from y0 to y1 at (a, d): a brass base and capital, and a
 * rosewood shaft between them swelling a little a third of the way up, as a
 * turner's column does.
 */
static void column(Kit* kit, const KitFrame* f, float a, float d, float y0, float y1, float r) {
    const float cap = 1.6f * r, len = (y1 - y0) - 2.0f * cap;
    const vec2 base[] = {{0.0f, 0.0f},         {1.35f * r, 0.0f},     {1.35f * r, 0.5f * r},
                         {1.1f * r, 0.8f * r}, {1.2f * r, 1.15f * r}, {r, cap},
                         {0.0f, cap}};
    const vec2 shaft[] = {
        {0.0f, 0.0f}, {r, 0.0f}, {1.05f * r, 0.35f * len}, {0.9f * r, len}, {0.0f, len}};
    const vec2 capital[] = {
        {0.0f, 0.0f},           {0.9f * r, 0.0f}, {1.2f * r, 0.4f * r}, {1.1f * r, 0.75f * r},
        {1.35f * r, 1.05f * r}, {1.35f * r, cap}, {0.0f, cap}};
    kit_frame_lathe(kit, f, MAT_BRASS, a, d, y0, base, COUNT(base), 16);
    kit_frame_lathe(kit, f, MAT_ROSEWOOD, a, d, y0 + cap, shaft, COUNT(shaft), 16);
    kit_frame_lathe(kit, f, MAT_BRASS, a, d, y0 + cap + len, capital, COUNT(capital), 16);
}

// A brass ball-and-spire finial standing at (a, y, d), `s` its scale.
static void finial(Kit* kit, const KitFrame* f, float a, float y, float d, float s) {
    const vec2 p[] = {{0.0f, 0.0f},
                      {0.02f * s, 0.0f},
                      {0.02f * s, 0.008f * s},
                      {0.011f * s, 0.013f * s},
                      {0.024f * s, 0.028f * s},
                      {0.027f * s, 0.043f * s},
                      {0.02f * s, 0.058f * s},
                      {0.008f * s, 0.065f * s},
                      {0.006f * s, 0.09f * s},
                      {0.0025f * s, 0.118f * s},
                      {0.0f, 0.125f * s}};
    kit_frame_lathe(kit, f, MAT_BRASS, a, d, y, p, COUNT(p), 16);
}

// A brass rosette facing out of the case at (a, y, d).
static void rosette(Kit* kit, const KitFrame* f, float a, float y, float d, float r) {
    const vec2 p[] = {{0.0f, 0.0f},          {r, 0.0f},
                      {r, 0.25f * r},        {0.75f * r, 0.4f * r},
                      {0.55f * r, 0.5f * r}, {0.3f * r, 0.75f * r},
                      {0.0f, 0.8f * r}};
    kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){a, y, d}, (vec3){0.0f, 0.0f, 1.0f}, p, COUNT(p),
                       20);
}

// One swan neck: a cubic from the cornice's outer corner, rising and turning
// in over the centre, at depth d. `side` is -1 or +1.
#define NECK_POINTS 14
static void neck_path(float side, float d, vec3 out[NECK_POINTS]) {
    const float pa[4] = {0.275f, 0.19f, 0.15f, 0.075f}, py[4] = {2.25f, 2.25f, 2.375f, 2.36f};
    for (int i = 0; i < NECK_POINTS; i++) {
        const float t = (float)i / (float)(NECK_POINTS - 1), u = 1.0f - t;
        const float w[4] = {u * u * u, 3.0f * u * u * t, 3.0f * u * t * t, t * t * t};
        out[i][0] = side * (w[0] * pa[0] + w[1] * pa[1] + w[2] * pa[2] + w[3] * pa[3]);
        out[i][1] = w[0] * py[0] + w[1] * py[1] + w[2] * py[2] + w[3] * py[3];
        out[i][2] = d;
    }
}

/*
 * The broken pediment: two swan necks sweeping up to brass rosettes, the
 * board beneath them stepped to follow their curve, and a finial on a plinth
 * in the gap between.
 */
static void pediment(Kit* kit, const KitFrame* f) {
    for (int side = -1; side <= 1; side += 2) {
        vec3 front[NECK_POINTS], back[NECK_POINTS];
        neck_path((float)side, 0.30f, front);
        neck_path((float)side, 0.05f, back);
        kit_frame_pipe(kit, f, MAT_ROSEWOOD, front, NECK_POINTS, 0.016f, 14);
        kit_frame_pipe(kit, f, MAT_ROSEWOOD, back, NECK_POINTS, 0.016f, 14);
        for (int i = 0; i + 1 < NECK_POINTS; i++) {
            const float top = fminf(front[i][1], front[i + 1][1]) - 0.004f;
            kit_frame_box(kit, f, MAT_ROSEWOOD, front[i][0], front[i + 1][0], 2.24f, top, 0.05f,
                          0.30f, false);
        }
        rosette(kit, f, 0.075f * (float)side, 2.36f, 0.312f, 0.024f);
        kit_frame_box(kit, f, MAT_ROSEWOOD, 0.23f * (float)side, 0.27f * (float)side, 2.24f, 2.265f,
                      0.25f, 0.30f, false);
        finial(kit, f, 0.25f * (float)side, 2.265f, 0.275f, 0.7f);
    }
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.035f, 0.035f, 2.24f, 2.30f, 0.13f, 0.27f, false);
    finial(kit, f, 0.0f, 2.30f, 0.20f, 1.0f);
}

// A pane in a door, and maple stringing inlaid round it on the door's face.
static void glazed(Kit* kit, const KitFrame* f, float a, float y0, float y1, float d, float face) {
    kit_frame_box(kit, f, MAT_GLASS_CLEAR, -a, a, y0, y1, d - 0.0015f, d + 0.0015f, false);
    const float in = 0.012f, w = 0.004f, a0 = a + in, lo = y0 - in, hi = y1 + in;
    kit_frame_box(kit, f, MAT_MAPLE, -a0 - w, -a0, lo - w, hi + w, face, face + 0.0012f, false);
    kit_frame_box(kit, f, MAT_MAPLE, a0, a0 + w, lo - w, hi + w, face, face + 0.0012f, false);
    kit_frame_box(kit, f, MAT_MAPLE, -a0, a0, lo - w, lo, face, face + 0.0012f, false);
    kit_frame_box(kit, f, MAT_MAPLE, -a0, a0, hi, hi + w, face, face + 0.0012f, false);
}

/*
 * A weight on its line: a brass cylinder domed at both ends, hung from a
 * pulley by a doubled gut line that runs up out of sight into the movement.
 */
static void weight(Kit* kit, const KitFrame* f, float a, float y) {
    const vec2 p[] = {{0.0f, 0.0f},     {0.022f, 0.0f},   {0.03f, 0.012f},  {0.03f, 0.19f},
                      {0.022f, 0.202f}, {0.007f, 0.206f}, {0.007f, 0.225f}, {0.0f, 0.225f}};
    kit_frame_lathe(kit, f, MAT_BRASS, a, 0.07f, y, p, COUNT(p), 20);
    for (int s = -1; s <= 1; s += 2) {
        const float la = a + 0.012f * (float)s;
        kit_frame_pipe(kit, f, MAT_BLACK, (vec3[]){{la, y + 0.225f, 0.07f}, {la, WAIST_Y1, 0.07f}},
                       2, 0.0018f, 6);
    }
}

void clock_build(Kit* kit) {
    const KitFrame* f = &CLOCK;

    // The plinth: bracket feet, a skirting, the marquetry panel, and the
    // mouldings stepping in to the waist.
    for (int s = -1; s <= 1; s += 2)
        for (int back = 0; back < 2; back++)
            kit_frame_box(kit, f, MAT_CASE, 0.18f * (float)s, 0.27f * (float)s, 0.0f, 0.07f,
                          back ? 0.0f : 0.21f, back ? 0.09f : 0.30f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.28f, 0.28f, 0.07f, 0.11f, 0.0f, 0.31f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.27f, 0.27f, 0.11f, 0.46f, 0.0f, 0.30f, false);
    const CardSpec* panel = &CARDS[CARD_CLOCK_PANEL];
    kit_frame_card(
        kit, f, MAT_CARDS, (vec3){-0.5f * panel->size[0], 0.285f - 0.5f * panel->size[1], 0.3008f},
        (vec3){panel->size[0], 0.0f, 0.0f}, (vec3){0.0f, panel->size[1], 0.0f}, panel->uv);
    kit_frame_box(kit, f, MAT_CASE, -0.28f, 0.28f, 0.46f, 0.49f, 0.0f, 0.31f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.255f, 0.255f, 0.49f, 0.525f, 0.0f, 0.285f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.235f, 0.235f, 0.525f, WAIST_Y0, 0.0f, 0.265f, false);

    // The waist: a hollow trunk with a glazed door, so the pendulum and the
    // weights show, quarter-columns at its front corners, and a keyhole.
    kit_frame_box(kit, f, MAT_CASE, -0.22f, 0.22f, WAIST_Y0, WAIST_Y1, 0.0f, 0.015f, false);
    for (int s = -1; s <= 1; s += 2) {
        kit_frame_box(kit, f, MAT_CASE, 0.205f * (float)s, 0.22f * (float)s, WAIST_Y0, WAIST_Y1,
                      0.0f, 0.25f, false);
        kit_frame_box(kit, f, MAT_ROSEWOOD, 0.15f * (float)s, 0.19f * (float)s, 0.60f, 1.46f,
                      0.235f, 0.255f, false);
        column(kit, f, 0.205f * (float)s, 0.24f, 0.60f, 1.46f, 0.016f);
        weight(kit, f, 0.10f * (float)s, 1.04f);
    }
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.22f, 0.22f, WAIST_Y0, 0.60f, 0.235f, 0.25f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.22f, 0.22f, 1.46f, WAIST_Y1, 0.235f, 0.25f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.15f, 0.15f, 0.60f, 0.66f, 0.235f, 0.255f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.15f, 0.15f, 1.38f, 1.46f, 0.235f, 0.255f, false);
    glazed(kit, f, 0.15f, 0.66f, 1.38f, 0.245f, 0.255f);
    kit_frame_box(kit, f, MAT_BRASS, 0.162f, 0.178f, 0.98f, 1.02f, 0.255f, 0.258f, false);
    kit_frame_box(kit, f, MAT_BLACK, 0.168f, 0.172f, 0.99f, 1.008f, 0.258f, 0.259f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.235f, 0.235f, WAIST_Y1, 1.53f, 0.0f, 0.265f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.255f, 0.255f, 1.53f, HOOD_Y0, 0.0f, 0.285f, false);

    // The hood: the painted dial behind a glazed door, turned columns either
    // side, the fretted frieze and the cornice over it.
    kit_frame_box(kit, f, MAT_CASE, -0.26f, 0.26f, HOOD_Y0, HOOD_Y1, 0.0f, 0.015f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.245f, 0.245f, HOOD_Y0, HOOD_Y1, 0.232f, 0.25f, false);
    const CardSpec* dial = &CARDS[CARD_CLOCK_DIAL];
    kit_frame_card(kit, f, MAT_CARDS, (vec3){-0.5f * dial->size[0], DIAL_Y0, DIAL_D},
                   (vec3){dial->size[0], 0.0f, 0.0f}, (vec3){0.0f, dial->size[1], 0.0f}, dial->uv);
    for (int s = -1; s <= 1; s += 2) {
        kit_frame_box(kit, f, MAT_ROSEWOOD, 0.245f * (float)s, 0.26f * (float)s, HOOD_Y0, HOOD_Y1,
                      0.0f, 0.30f, false);
        kit_frame_box(kit, f, MAT_ROSEWOOD, 0.17f * (float)s, 0.20f * (float)s, HOOD_Y0, HOOD_Y1,
                      0.27f, 0.295f, false);
        kit_frame_box(kit, f, MAT_CASE, 0.20f * (float)s, 0.245f * (float)s, HOOD_Y0, HOOD_Y1,
                      0.25f, 0.265f, false);
        column(kit, f, 0.225f * (float)s, 0.28f, HOOD_Y0, HOOD_Y1, 0.018f);
    }
    const float dial_top = DIAL_Y0 + dial->size[1];
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.17f, 0.17f, HOOD_Y0, DIAL_Y0, 0.27f, 0.295f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.17f, 0.17f, dial_top, HOOD_Y1, 0.27f, 0.295f, false);
    glazed(kit, f, 0.17f, DIAL_Y0, dial_top, 0.2805f, 0.295f);
    // The collet holding the hands on, in front of the minute hand.
    const vec2 collet[] = {
        {0.0f, 0.0f}, {0.007f, 0.0f}, {0.007f, 0.003f}, {0.004f, 0.006f}, {0.0f, 0.007f}};
    const float hands_y = DIAL_Y0 + CARD_CLOCK_DIAL_HANDS[1];
    kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){0.0f, hands_y, DIAL_D + 0.0095f},
                       (vec3){0.0f, 0.0f, 1.0f}, collet, COUNT(collet), 16);

    const CardSpec* frieze = &CARDS[CARD_CLOCK_FRIEZE];
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.26f, 0.26f, HOOD_Y1, 2.17f, 0.0f, 0.30f, false);
    kit_frame_card(kit, f, MAT_CARDS, (vec3){-0.5f * frieze->size[0], HOOD_Y1, 0.3008f},
                   (vec3){frieze->size[0], 0.0f, 0.0f}, (vec3){0.0f, frieze->size[1], 0.0f},
                   frieze->uv);
    kit_frame_box(kit, f, MAT_CASE, -0.27f, 0.27f, 2.17f, 2.19f, 0.0f, 0.31f, false);
    kit_frame_box(kit, f, MAT_ROSEWOOD, -0.28f, 0.28f, 2.19f, 2.215f, 0.0f, 0.32f, false);
    kit_frame_box(kit, f, MAT_CASE, -0.28f, 0.28f, 2.215f, 2.24f, 0.0f, 0.325f, false);
    pediment(kit, f);

    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -0.28f, 0.28f, 0.0f, 2.30f, 0.0f, 0.325f, true);
}

// The hands are cut from sheet steel, so each is a few flat polygons facing out of the case.
// The hour hand: a stem to a spade, and a round tail to balance it.
static void hour_hand(Kit* kit, const KitFrame* f) {
    const vec2 stem[] = {{-0.004f, -0.02f}, {0.004f, -0.02f}, {0.003f, 0.06f}, {-0.003f, 0.06f}};
    const vec2 spade[] = {{0.0f, 0.052f}, {0.016f, 0.074f}, {0.0f, 0.108f}, {-0.016f, 0.074f}};
    const vec2 tail[] = {{0.0f, -0.034f}, {0.009f, -0.028f},  {0.009f, -0.016f},
                         {0.0f, -0.01f},  {-0.009f, -0.016f}, {-0.009f, -0.028f}};
    kit_frame_polygon(kit, f, MAT_BLACK, stem, COUNT(stem), 0.004f);
    kit_frame_polygon(kit, f, MAT_BLACK, spade, COUNT(spade), 0.004f);
    kit_frame_polygon(kit, f, MAT_BLACK, tail, COUNT(tail), 0.004f);
}

// The minute hand: long and fine, a lozenge near its tip.
static void minute_hand(Kit* kit, const KitFrame* f) {
    const vec2 stem[] = {
        {-0.004f, -0.035f}, {0.004f, -0.035f}, {0.0018f, 0.17f}, {-0.0018f, 0.17f}};
    const vec2 lozenge[] = {{0.0f, 0.12f}, {0.007f, 0.135f}, {0.0f, 0.15f}, {-0.007f, 0.135f}};
    const vec2 tip[] = {{-0.0018f, 0.17f}, {0.0018f, 0.17f}, {0.0f, 0.192f}};
    kit_frame_polygon(kit, f, MAT_BLACK, stem, COUNT(stem), 0.0075f);
    kit_frame_polygon(kit, f, MAT_BLACK, lozenge, COUNT(lozenge), 0.0075f);
    kit_frame_polygon(kit, f, MAT_BLACK, tip, COUNT(tip), 0.0075f);
}

static void second_hand(Kit* kit, const KitFrame* f) {
    const vec2 needle[] = {
        {-0.0012f, -0.01f}, {0.0012f, -0.01f}, {0.0006f, 0.036f}, {-0.0006f, 0.036f}};
    kit_frame_polygon(kit, f, MAT_BLACK, needle, COUNT(needle), 0.003f);
}

/*
 * The pendulum, hung from its pivot: a suspension spring, a long steel rod,
 * and a lenticular brass bob turned on its side to face the room.
 */
static void pendulum(Kit* kit, const KitFrame* f) {
    kit_frame_box(kit, f, MAT_BLACK, -0.006f, 0.006f, -0.035f, 0.0f, -0.001f, 0.001f, false);
    kit_frame_pipe(kit, f, MAT_STEEL,
                   (vec3[]){{0.0f, -0.035f, 0.0f}, {0.0f, -PENDULUM_L + 0.07f, 0.0f}}, 2, 0.0035f,
                   8);
    const vec2 bob[] = {{0.0f, 0.0f},     {0.05f, 0.004f}, {0.078f, 0.01f}, {0.082f, 0.012f},
                        {0.078f, 0.014f}, {0.05f, 0.02f},  {0.0f, 0.024f}};
    kit_frame_lathe_on(kit, f, MAT_BRASS, (vec3){0.0f, -PENDULUM_L, BOB_D},
                       (vec3){0.0f, 0.0f, 1.0f}, bob, COUNT(bob), 32);
}

// A moving part as a node of its own: a small kit of its own, built round the
// part's pivot at the origin in the case's orientation, so the node's
// transform is the pivot's place and the part's turn and nothing else.
static SceneNode* part(Engine* engine, Scene* scene, const char* name,
                       void (*build)(Kit*, const KitFrame*)) {
    Kit kit;
    kit_init(&kit, scene, NULL, NULL);
    mats_register(&kit, engine, scene);
    const KitFrame f = {{0.0f, 0.0f, 0.0f}, CLOCK.yaw};
    build(&kit, &f);
    return kit_finish(&kit, name);
}

// Where a part turns about, in the world, and the axis it turns about.
static void pivot_at(float y, float d, vec3 pivot) {
    kit_frame_point(&CLOCK, 0.0f, y, d, pivot);
}

// A part's transform: its pivot's place, turned by `angle` about the case's d.
static void part_transform(float y, float d, float angle, mat4 out) {
    vec3 pivot = {0.0f, 0.0f, 0.0f}, axis = {0.0f, 0.0f, 0.0f};
    pivot_at(y, d, pivot);
    kit_frame_dir(&CLOCK, 0.0f, 0.0f, 1.0f, axis);
    glm_translate_make(out, pivot);
    glm_rotate(out, angle, axis);
}

static void turn(SceneNode* node, float y, float d, float angle) {
    if (node)
        part_transform(y, d, angle, node->original_transform);
}

static float swing_at(double time) {
    return SWING * (float)sin(2.0 * GLM_PI * time / PERIOD);
}

void clock_bob(double time, vec3 out) {
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    part_transform(PIVOT_Y, PIVOT_D, swing_at(time), m);
    const KitFrame f = {{0.0f, 0.0f, 0.0f}, CLOCK.yaw};
    vec3 bob = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&f, 0.0f, -PENDULUM_L, BOB_D, bob);
    glm_mat4_mulv3(m, bob, 1.0f, out);
}

static Sound* beat_sound(AudioSystem* audio, const char* path) {
    Sound* s = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    if (!s) {
        fprintf(stderr, "silent: the clock cannot load %s\n", path);
        return NULL;
    }
    vec3 at = {0.0f, 0.0f, 0.0f};
    pivot_at(1.75f, 0.12f, at);
    audio_sound_set_position(s, at);
    audio_sound_set_volume(s, BEAT_VOLUME);
    return s;
}

void clock_start(Clock* clock, Engine* engine, Scene* scene, AudioSystem* audio) {
    clock->pendulum = part(engine, scene, "clock_pendulum", pendulum);
    clock->hour = part(engine, scene, "clock_hour_hand", hour_hand);
    clock->minute = part(engine, scene, "clock_minute_hand", minute_hand);
    clock->second = part(engine, scene, "clock_second_hand", second_hand);
    clock->tick = audio ? beat_sound(audio, "assets/audio/silent/tick.wav") : NULL;
    clock->tock = audio ? beat_sound(audio, "assets/audio/silent/tock.wav") : NULL;
    clock->beat = -1;
}

/*
 * The escapement lets the train move once a beat, at each end of the swing,
 * where the pendulum stops: so that is when the clock ticks and the seconds
 * hand steps. The other hands creep, geared down from the same train.
 */
void clock_update(Clock* clock, double time, float hearing) {
    turn(clock->pendulum, PIVOT_Y, PIVOT_D, swing_at(time));

    const long beat = (long)floor(time * 2.0 / PERIOD + 0.5);
    if (clock->beat >= 0 && beat != clock->beat) {
        Sound* s = (beat & 1) ? clock->tock : clock->tick;
        if (s) {
            audio_sound_set_volume(s, BEAT_VOLUME * hearing);
            audio_sound_play(s);
        }
    }
    clock->beat = beat;

    const double shown = START_TIME + time;
    const double seconds = fmod(floor(START_TIME) + (double)beat, 60.0);
    const double minutes = fmod(shown / 60.0, 60.0);
    const double hours = fmod(shown / 3600.0, 12.0);
    // Clockwise seen from the hall is a negative turn about the case's d.
    const float hands_y = DIAL_Y0 + CARD_CLOCK_DIAL_HANDS[1];
    const float seconds_y = DIAL_Y0 + CARD_CLOCK_DIAL_SECONDS[1];
    turn(clock->hour, hands_y, DIAL_D, -(float)(hours / 12.0 * 2.0 * GLM_PI));
    turn(clock->minute, hands_y, DIAL_D, -(float)(minutes / 60.0 * 2.0 * GLM_PI));
    turn(clock->second, seconds_y, DIAL_D, -(float)(seconds / 60.0 * 2.0 * GLM_PI));
}
