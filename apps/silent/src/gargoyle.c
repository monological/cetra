#include "gargoyle.h"
#include "mats.h"

// Lathed parts are coarse on purpose: the beast is seen from the street, nine metres down.
#define SIDES 10

/*
 * The sculpt is drawn in its own metres at SCULPT_SCALE: every point and every profile below
 * is multiplied by it on the way to the kit, so the beast's proportions are one table of
 * numbers and its size is one more. At 1.5 it is about 1.2 m from the wall to its jaws, a
 * tower gargoyle's size, and reads from the yard.
 */
#define SCULPT_SCALE 1.5f

// A point of the sculpt in the frame's (a, y, d).
static void at(float a, float y, float d, vec3 out) {
    glm_vec3_copy((vec3){a * SCULPT_SCALE, y * SCULPT_SCALE, d * SCULPT_SCALE}, out);
}

// A unit vector in the frame's (a, y, d); directions do not scale.
static void unit(float a, float y, float d, vec3 out) {
    glm_vec3_copy((vec3){a, y, d}, out);
    glm_vec3_normalize(out);
}

// A profile or outline of the sculpt, scaled into `out`.
static void scaled(const vec2* in, int count, vec2* out) {
    for (int i = 0; i < count; i++)
        glm_vec2_scale((float*)in[i], SCULPT_SCALE, out[i]);
}

static void lathe(Kit* kit, const KitFrame* f, int mat, const vec3 base, const vec3 dir,
                  const vec2* profile, int count, int sides) {
    vec2 p[KIT_MAX_POINTS];
    scaled(profile, count, p);
    kit_frame_lathe_on(kit, f, mat, base, dir, p, count, sides);
}

// A cone `len` long, `r` across its base, from `base` along `dir`: a horn, a fang, a claw.
static void cone(Kit* kit, const KitFrame* f, const vec3 base, const vec3 dir, float r, float len) {
    const vec2 p[] = {{0.0f, 0.0f}, {r, 0.0f}, {0.6f * r, 0.45f * len}, {0.0f, len}};
    lathe(kit, f, MAT_STONE, base, dir, p, KIT_COUNT(p), 6);
}

// The box a0..a1, y0..y1, d0..d1 of the sculpt.
static void block(Kit* kit, const KitFrame* f, float a0, float a1, float y0, float y1, float d0,
                  float d1) {
    const float s = SCULPT_SCALE;
    kit_frame_box(kit, f, MAT_STONE, a0 * s, a1 * s, y0 * s, y1 * s, d0 * s, d1 * s, false);
}

// The perch: the spout stone it crouches on, run out of the wall to carry the water clear.
static void perch(Kit* kit, const KitFrame* f) {
    block(kit, f, -0.16f, 0.16f, -0.32f, -0.18f, 0.0f, 0.62f);
}

static void body(Kit* kit, const KitFrame* f) {
    vec3 axis = {0.0f, 0.0f, 0.0f}, p = {0.0f, 0.0f, 0.0f};
    // The torso, an egg leaning out and up from its haunches.
    unit(0.0f, 0.35f, 1.0f, axis);
    const vec2 torso[] = {{0.0f, 0.0f},   {0.1f, 0.02f},  {0.15f, 0.1f}, {0.17f, 0.22f},
                          {0.16f, 0.34f}, {0.12f, 0.44f}, {0.07f, 0.5f}, {0.0f, 0.52f}};
    at(0.0f, -0.1f, 0.02f, p);
    lathe(kit, f, MAT_STONE, p, axis, torso, KIT_COUNT(torso), SIDES);
    // A ridge of knobs down its spine.
    const vec3 up = {0.0f, axis[2], -axis[1]};
    for (int i = 0; i < 3; i++) {
        const float t = 0.14f + 0.1f * (float)i, r = 0.16f;
        at(0.0f, -0.1f + axis[1] * t + up[1] * r, 0.02f + axis[2] * t + up[2] * r, p);
        cone(kit, f, p, up, 0.022f, 0.07f);
    }
    // The haunches either side, folded under it, and the hind feet on the stone.
    const vec2 haunch[] = {{0.0f, 0.0f},   {0.06f, 0.02f}, {0.09f, 0.08f},
                           {0.08f, 0.16f}, {0.05f, 0.21f}, {0.0f, 0.23f}};
    for (int s = -1; s <= 1; s += 2) {
        unit(0.15f * (float)s, 0.2f, 1.0f, axis);
        at(0.11f * (float)s, -0.12f, 0.0f, p);
        lathe(kit, f, MAT_STONE, p, axis, haunch, KIT_COUNT(haunch), SIDES);
        block(kit, f, 0.08f * (float)s, 0.17f * (float)s, -0.18f, -0.13f, 0.12f, 0.26f);
    }
}

static void legs(Kit* kit, const KitFrame* f) {
    vec3 down = {0.0f, 0.0f, 0.0f}, p = {0.0f, 0.0f, 0.0f};
    unit(0.0f, -0.7f, 1.0f, down);
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s;
        vec3 arm[4];
        at(0.12f * a, 0.0f, 0.36f, arm[0]);
        at(0.15f * a, -0.08f, 0.44f, arm[1]);
        at(0.14f * a, -0.14f, 0.5f, arm[2]);
        at(0.12f * a, -0.17f, 0.53f, arm[3]);
        kit_frame_pipe(kit, f, MAT_STONE, arm, 4, 0.034f * SCULPT_SCALE, 8);
        block(kit, f, 0.08f * a, 0.16f * a, -0.18f, -0.15f, 0.5f, 0.58f);
        // Three claws over the stone's edge.
        for (int c = -1; c <= 1; c++) {
            at((0.12f + 0.025f * (float)c) * a, -0.165f, 0.575f, p);
            cone(kit, f, p, down, 0.011f, 0.05f);
        }
    }
}

static void head(Kit* kit, const KitFrame* f) {
    vec3 axis = {0.0f, 0.0f, 0.0f}, p = {0.0f, 0.0f, 0.0f};
    unit(0.0f, -0.15f, 1.0f, axis);
    // Skull to snout, along a line dipping toward the street.
    const vec2 skull[] = {{0.0f, 0.0f},   {0.07f, 0.01f}, {0.105f, 0.06f},
                          {0.11f, 0.12f}, {0.09f, 0.18f}, {0.06f, 0.24f},
                          {0.045f, 0.3f}, {0.03f, 0.33f}, {0.0f, 0.34f}};
    at(0.0f, 0.04f, 0.42f, p);
    lathe(kit, f, MAT_STONE, p, axis, skull, KIT_COUNT(skull), SIDES);
    // The lower jaw, dropped open: the spout's lip.
    const vec2 jaw[] = {{0.52f, -0.02f}, {0.74f, -0.08f}, {0.76f, -0.115f}, {0.55f, -0.075f}};
    vec2 jaw_s[KIT_COUNT(jaw)];
    scaled(jaw, KIT_COUNT(jaw), jaw_s);
    kit_frame_run(kit, f, MAT_STONE, jaw_s, KIT_COUNT(jaw), -0.045f * SCULPT_SCALE,
                  0.045f * SCULPT_SCALE);
    vec3 fang = {0.0f, 0.0f, 0.0f};
    unit(0.0f, -1.0f, 0.15f, fang);
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s;
        // Fangs down from the upper jaw, and up from the lower.
        at(0.028f * a, -0.005f, 0.69f, p);
        cone(kit, f, p, fang, 0.01f, 0.045f);
        at(0.022f * a, -0.075f, 0.71f, p);
        cone(kit, f, p, (vec3){0.0f, 1.0f, 0.0f}, 0.008f, 0.035f);
        // Horns swept back off the brow.
        vec3 horn = {0.0f, 0.0f, 0.0f};
        unit(0.35f * a, 1.0f, -0.55f, horn);
        at(0.06f * a, 0.12f, 0.5f, p);
        cone(kit, f, p, horn, 0.026f, 0.2f);
        // Eyes sunk under the brow.
        const vec2 eye[] = {{0.0f, 0.0f}, {0.02f, 0.006f}, {0.02f, 0.022f}, {0.0f, 0.028f}};
        at(0.05f * a, 0.06f, 0.6f, p);
        lathe(kit, f, MAT_BLACK, p, axis, eye, KIT_COUNT(eye), 6);
    }
}

// Bat wings folded up behind the shoulders: a thin membrane between scalloped fingers, and the
// fingers' bones along it.
static void wings(Kit* kit, const KitFrame* f) {
    static const vec2 WING[] = {{0.4f, 0.02f},  {0.36f, 0.22f}, {0.3f, 0.44f},  {0.24f, 0.62f},
                                {0.18f, 0.48f}, {0.1f, 0.52f},  {0.06f, 0.36f}, {-0.02f, 0.4f},
                                {0.0f, 0.2f},   {0.06f, 0.04f}, {0.2f, 0.0f}};
    static const int TIPS[] = {3, 5, 7}; // the fingers' ends among WING's points
    vec2 wing[KIT_COUNT(WING)];
    scaled(WING, KIT_COUNT(WING), wing);
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s;
        kit_frame_run(kit, f, MAT_STONE, wing, KIT_COUNT(WING), 0.13f * a * SCULPT_SCALE,
                      0.15f * a * SCULPT_SCALE);
        for (int t = 0; t < KIT_COUNT(TIPS); t++) {
            vec3 bone[2];
            at(0.155f * a, 0.04f, 0.38f, bone[0]);
            at(0.155f * a, WING[TIPS[t]][1], WING[TIPS[t]][0], bone[1]);
            kit_frame_pipe(kit, f, MAT_STONE, bone, 2, 0.012f * SCULPT_SCALE, 5);
        }
    }
}

void gargoyle_build(Kit* kit, const KitFrame* f, vec3 mouth) {
    perch(kit, f);
    body(kit, f);
    legs(kit, f);
    head(kit, f);
    wings(kit, f);
    vec3 lip = {0.0f, 0.0f, 0.0f};
    at(0.0f, -0.05f, 0.76f, lip);
    kit_frame_point(f, lip[0], lip[1], lip[2], mouth);
}
