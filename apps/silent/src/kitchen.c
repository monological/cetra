#include <math.h>
#include <stdint.h>

#include "cards.h"
#include "kitchen.h"
#include "layout.h"
#include "mats.h"
#include "sounds.h"

/*
 * The kitchen, furnished. Heights are above the floor and every run is laid in
 * a KitFrame against its wall, so "along", "up" and "out" read the same on all
 * three: the window wall ahead as you come in, the hall wall on the left with
 * the stove, and the outside wall on the right with the shelves and the fridge.
 */

#define CARCASS_TOP 0.86f // the counter sits on this
#define COUNTER_TOP 0.90f
#define COUNTER_D   0.62f
#define CARCASS_D   0.58f
#define PLINTH      0.10f
#define UPPER_Y0    1.50f
#define UPPER_Y1    2.25f
#define UPPER_D     0.34f
#define UNIT_W      0.60f

// The sink, centred under the window, in the window wall's frame.
#define SINK_A0 2.15f
#define SINK_A1 2.95f
#define SINK_D0 0.10f
#define SINK_D1 0.50f

// The mat in front of the stove: its middle and half its size.
static const vec3 STOVE_MAT = {1.12f, FLOOR_Y + 0.004f, 11.72f};
static const vec3 STOVE_MAT_HALF = {0.38f, 0.004f, 0.62f};

/*
 * Base cabinets from a0 to a1: a dark recessed plinth, the carcass, a door per
 * unit with a drawer over every other one, and bar handles. The carcass drops
 * to `hole_top` between hole0 and hole1, which is where a sink basin goes.
 */
static void base_units(Kit* kit, const KitFrame* f, float a0, float a1, float depth, float hole0,
                       float hole1, float hole_top) {
    kit_frame_box(kit, f, MAT_BLACK, a0, a1, 0.0f, PLINTH, 0.0f, depth - 0.05f, false);
    if (hole1 > hole0) {
        kit_frame_box(kit, f, MAT_ENAMEL, a0, hole0, PLINTH, CARCASS_TOP, 0.0f, depth, false);
        kit_frame_box(kit, f, MAT_ENAMEL, hole0, hole1, PLINTH, hole_top, 0.0f, depth, false);
        kit_frame_box(kit, f, MAT_ENAMEL, hole1, a1, PLINTH, CARCASS_TOP, 0.0f, depth, false);
    } else {
        kit_frame_box(kit, f, MAT_ENAMEL, a0, a1, PLINTH, CARCASS_TOP, 0.0f, depth, false);
    }
    const int n = (int)fmaxf(1.0f, roundf((a1 - a0) / UNIT_W));
    const float w = (a1 - a0) / (float)n;
    for (int i = 0; i < n; i++) {
        const float u0 = a0 + (float)i * w, u1 = u0 + w;
        const bool drawer = (i & 1) == 0;
        const float door_top = drawer ? CARCASS_TOP - 0.2f : CARCASS_TOP - 0.03f;
        kit_frame_box(kit, f, MAT_ENAMEL, u0 + 0.008f, u1 - 0.008f, PLINTH + 0.03f, door_top, depth,
                      depth + 0.02f, false);
        const float ha = (i & 1) ? u0 + 0.05f : u1 - 0.05f;
        kit_frame_prism(kit, f, MAT_STEEL, ha, depth + 0.04f, door_top - 0.24f, door_top - 0.07f,
                        0.008f, 6);
        if (drawer) {
            kit_frame_box(kit, f, MAT_ENAMEL, u0 + 0.008f, u1 - 0.008f, CARCASS_TOP - 0.17f,
                          CARCASS_TOP - 0.03f, depth, depth + 0.02f, false);
            kit_frame_bar(kit, f, MAT_STEEL, u0 + w * 0.35f, u1 - w * 0.35f, CARCASS_TOP - 0.1f,
                          depth + 0.04f, 0.008f);
        }
    }
    // One body for the whole run, counter and all.
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a0, a1, 0.0f, COUNTER_TOP, 0.0f, depth + 0.04f, true);
}

// Wall cabinets: a box and two doors, their handles at the bottom inner edges.
static void upper_units(Kit* kit, const KitFrame* f, float a0, float a1) {
    kit_frame_box(kit, f, MAT_ENAMEL, a0, a1, UPPER_Y0, UPPER_Y1, 0.0f, UPPER_D, false);
    const int n = (int)fmaxf(1.0f, roundf((a1 - a0) / (UNIT_W * 0.8f)));
    const float w = (a1 - a0) / (float)n;
    for (int i = 0; i < n; i++) {
        const float u0 = a0 + (float)i * w, u1 = u0 + w;
        kit_frame_box(kit, f, MAT_ENAMEL, u0 + 0.008f, u1 - 0.008f, UPPER_Y0 + 0.02f,
                      UPPER_Y1 - 0.02f, UPPER_D, UPPER_D + 0.02f, false);
        const float ha = (i & 1) ? u0 + 0.05f : u1 - 0.05f;
        kit_frame_prism(kit, f, MAT_STEEL, ha, UPPER_D + 0.04f, UPPER_Y0 + 0.06f, UPPER_Y0 + 0.22f,
                        0.008f, 6);
    }
}

static void counter(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float d0, float d1) {
    kit_frame_box(kit, f, mat, a0, a1, CARCASS_TOP, COUNTER_TOP, d0, d1, false);
}

#define COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/*
 * Things on a surface at height y, in a frame: the clutter vocabulary. A plate
 * stands on a foot ring and its rim slopes up from the well; the profile runs
 * up the outside and back in over the top, so each nests in the one below.
 */
static const vec2 PLATE[] = {
    {0.0f, 0.003f},   {0.066f, 0.003f}, {0.068f, 0.0f},   {0.075f, 0.0f},  {0.079f, 0.004f},
    {0.118f, 0.014f}, {0.12f, 0.016f},  {0.117f, 0.017f}, {0.08f, 0.009f}, {0.0f, 0.008f},
};

void kitchen_plates(Kit* kit, const KitFrame* f, float a, float d, float y, int count) {
    for (int i = 0; i < count; i++)
        kit_frame_lathe(kit, f, MAT_CERAMIC, a, d, y + 0.012f * (float)i, PLATE, COUNT(PLATE), 32);
}

// A mug: open, with a wall you can see the thickness of at the lip, and a loop
// handle on its right.
static const vec2 MUG[] = {
    {0.0f, 0.0f},    {0.038f, 0.0f}, {0.042f, 0.004f}, {0.042f, 0.096f},
    {0.0405f, 0.1f}, {0.037f, 0.1f}, {0.037f, 0.012f}, {0.0f, 0.012f},
};

void kitchen_mug(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_lathe(kit, f, MAT_CERAMIC, a, d, y, MUG, COUNT(MUG), 24);
    enum { LOOP = 9 };
    vec3 handle[LOOP];
    for (int i = 0; i < LOOP; i++) {
        const float t = GLM_PIf * (0.5f - (float)i / (float)(LOOP - 1));
        glm_vec3_copy((vec3){a + 0.039f + 0.028f * cosf(t), y + 0.052f + 0.03f * sinf(t), d},
                      handle[i]);
    }
    kit_frame_pipe(kit, f, MAT_CERAMIC, handle, LOOP, 0.0055f, 10);
}

/*
 * Containers are glass shells with what is in them standing inside: an opaque
 * fill to some level, which the refraction pass sees through the glass. Brown
 * glass mostly, as in any old pantry, and some clear.
 */
static int glass(KitRng* rng) {
    return kit_rnd(rng) < 0.6f ? MAT_GLASS_AMBER : MAT_GLASS_CLEAR;
}

static int contents(KitRng* rng) {
    const float x = kit_rnd(rng);
    return x < 0.45f ? MAT_CONTENTS : (x < 0.75f ? MAT_CONTENTS_PALE : MAT_CONTENTS_GREEN);
}

// Filled part way, so there is empty glass above what is in it for the wall
// to show through: that is most of what reads as glass.
static void bottle(Kit* kit, const KitFrame* f, KitRng* rng, float a, float d, float y, float h) {
    const float r = 0.036f;
    const int g = glass(rng);
    kit_frame_prism(kit, f, g, a, d, y, y + h, r, 10);
    kit_frame_prism(kit, f, g, a, d, y + h, y + h + 0.08f, 0.013f, 8);
    kit_frame_prism(kit, f, MAT_CONTENTS, a, d, y + 0.004f, y + h * kit_rrange(rng, 0.15f, 0.6f),
                    r - 0.005f, 10);
    kit_frame_prism(kit, f, MAT_BLACK, a, d, y + h + 0.08f, y + h + 0.095f, 0.015f, 8);
}

static void jar(Kit* kit, const KitFrame* f, KitRng* rng, float a, float d, float y, float r,
                float h) {
    kit_frame_prism(kit, f, glass(rng), a, d, y, y + h, r, 12);
    kit_frame_prism(kit, f, contents(rng), a, d, y + 0.005f, y + h * kit_rrange(rng, 0.25f, 0.7f),
                    r - 0.006f, 12);
    kit_frame_prism(kit, f, MAT_STEEL, a, d, y + h, y + h + 0.02f, r * 0.92f, 12);
}

/*
 * A stainless stockpot, 20 cm across: a base rounding into the wall, a rim
 * rolled outward into a bead, and a domed lid with a black knob. The lid sits
 * inside the bead, so the pot's open inside is never seen and is not built.
 */
static const vec2 POT_BODY[] = {
    {0.0f, 0.0f},       {0.088f, 0.0f},   {0.096f, 0.003f},   {0.1f, 0.012f},
    {0.1f, 0.112f},     {0.103f, 0.114f}, {0.1058f, 0.1152f}, {0.107f, 0.118f},
    {0.1058f, 0.1208f}, {0.103f, 0.122f}, {0.1002f, 0.1208f}, {0.099f, 0.118f},
};
static const vec2 POT_LID[] = {
    {0.0f, 0.119f},   {0.101f, 0.119f}, {0.102f, 0.122f}, {0.098f, 0.126f},
    {0.085f, 0.132f}, {0.06f, 0.138f},  {0.03f, 0.141f},  {0.0f, 0.142f},
};
static const vec2 POT_KNOB[] = {
    {0.0f, 0.142f},   {0.008f, 0.142f}, {0.008f, 0.15f}, {0.017f, 0.156f},
    {0.018f, 0.162f}, {0.012f, 0.166f}, {0.0f, 0.167f},
};

static void pot(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_lathe(kit, f, MAT_STAINLESS, a, d, y, POT_BODY, COUNT(POT_BODY), 40);
    kit_frame_lathe(kit, f, MAT_STAINLESS, a, d, y, POT_LID, COUNT(POT_LID), 40);
    kit_frame_lathe(kit, f, MAT_BLACK, a, d, y, POT_KNOB, COUNT(POT_KNOB), 20);
    // A loop handle each side: half an ellipse out from the wall and back.
    enum { LOOP = 9 };
    for (int s = -1; s <= 1; s += 2) {
        vec3 path[LOOP];
        for (int i = 0; i < LOOP; i++) {
            const float t = GLM_PIf * ((float)i / (float)(LOOP - 1) - 0.5f);
            path[i][0] = a + (float)s * (0.098f + 0.036f * cosf(t));
            path[i][1] = y + 0.088f + 0.006f * cosf(t);
            path[i][2] = d + 0.032f * sinf(t);
        }
        kit_frame_pipe(kit, f, MAT_STAINLESS, path, LOOP, 0.005f, 12);
    }
}

/*
 * A gooseneck mixer tap: a flared base, a riser that turns through a half
 * circle out over the basin and down to an aerator, and a lever each side on
 * its own post.
 */
static const vec2 TAP_BASE[] = {
    {0.0f, 0.0f},     {0.03f, 0.0f},    {0.03f, 0.006f}, {0.026f, 0.013f},
    {0.017f, 0.019f}, {0.012f, 0.022f}, {0.0f, 0.023f},
};
static const vec2 TAP_POST[] = {
    {0.0f, 0.0f},     {0.018f, 0.0f},   {0.018f, 0.004f}, {0.015f, 0.008f},
    {0.015f, 0.032f}, {0.012f, 0.036f}, {0.0f, 0.037f},
};

static void faucet(Kit* kit, const KitFrame* f, float a, float d, float y) {
    const float r = 0.011f, rise = y + 0.26f, bend = 0.11f, spout = d + 2.0f * bend;
    enum { ARC = 13 };
    kit_frame_lathe(kit, f, MAT_STAINLESS, a, d, y, TAP_BASE, COUNT(TAP_BASE), 32);
    vec3 neck[ARC + 2];
    glm_vec3_copy((vec3){a, y + 0.02f, d}, neck[0]);
    for (int k = 0; k < ARC; k++) {
        const float t = GLM_PIf * (1.0f - (float)k / (float)(ARC - 1));
        glm_vec3_copy((vec3){a, rise + bend * sinf(t), d + bend + bend * cosf(t)}, neck[k + 1]);
    }
    glm_vec3_copy((vec3){a, rise - 0.04f, spout}, neck[ARC + 1]);
    kit_frame_pipe(kit, f, MAT_STAINLESS, neck, ARC + 2, r, 20);
    kit_frame_pipe(kit, f, MAT_STAINLESS,
                   (vec3[]){{a, rise - 0.028f, spout}, {a, rise - 0.048f, spout}}, 2, 0.0135f, 20);
    for (int s = -1; s <= 1; s += 2) {
        const float la = a + 0.1f * (float)s;
        kit_frame_lathe(kit, f, MAT_STAINLESS, la, d, y, TAP_POST, COUNT(TAP_POST), 24);
        kit_frame_pipe(
            kit, f, MAT_STAINLESS,
            (vec3[]){{la, y + 0.03f, d}, {la, y + 0.04f, d + 0.04f}, {la, y + 0.044f, d + 0.075f}},
            3, 0.0055f, 12);
    }
}

// A frying pan with its handle reaching out toward the room.
static void pan(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_prism(kit, f, MAT_BLACK, a, d, y, y + 0.04f, 0.12f, 12);
    kit_frame_box(kit, f, MAT_BLACK, a - 0.012f, a + 0.012f, y + 0.02f, y + 0.035f, d + 0.12f,
                  d + 0.30f, false);
}

// A block of knives against the wall, a handle standing out of each slot.
static void knife_block(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_box(kit, f, MAT_WOOD, a - 0.05f, a + 0.05f, y, y + 0.22f, d - 0.07f, d + 0.07f,
                  false);
    for (int i = 0; i < 4; i++) {
        const float ka = a - 0.03f + 0.02f * (float)i;
        const float top = y + 0.30f + 0.025f * (float)(i & 1);
        kit_frame_box(kit, f, MAT_BLACK, ka - 0.007f, ka + 0.007f, y + 0.22f, top, d - 0.015f,
                      d + 0.015f, false);
    }
}

// A knife lying along the wall: the blade from a0, the handle after it.
static void knife(Kit* kit, const KitFrame* f, float a0, float d, float y) {
    kit_frame_box(kit, f, MAT_STEEL, a0, a0 + 0.19f, y, y + 0.003f, d - 0.012f, d + 0.012f, false);
    kit_frame_box(kit, f, MAT_BLACK, a0 + 0.19f, a0 + 0.30f, y, y + 0.018f, d - 0.01f, d + 0.01f,
                  false);
}

static void cutting_board(Kit* kit, const KitFrame* f, float a0, float d0, float y) {
    kit_frame_box(kit, f, MAT_WOOD, a0, a0 + 0.38f, y, y + 0.02f, d0, d0 + 0.26f, false);
    knife(kit, f, a0 + 0.04f, d0 + 0.13f, y + 0.02f);
}

// A steel can of utensils, heads up: a plastic spatula and two wooden spoons.
static void utensil_can(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_prism(kit, f, MAT_STEEL, a, d, y, y + 0.16f, 0.055f, 10);
    kit_frame_prism(kit, f, MAT_BLACK, a + 0.018f, d + 0.01f, y + 0.02f, y + 0.30f, 0.007f, 6);
    kit_frame_box(kit, f, MAT_BLACK, a - 0.017f, a + 0.053f, y + 0.30f, y + 0.39f, d + 0.004f,
                  d + 0.016f, false);
    for (int i = 0; i < 2; i++) {
        const float sa = a - 0.02f + 0.012f * (float)i, sd = d - 0.015f + 0.02f * (float)i;
        const float top = y + 0.29f + 0.03f * (float)i;
        kit_frame_prism(kit, f, MAT_WOOD, sa, sd, y + 0.02f, top, 0.007f, 6);
        kit_frame_box(kit, f, MAT_WOOD, sa - 0.022f, sa + 0.022f, top, top + 0.06f, sd - 0.008f,
                      sd + 0.008f, false);
    }
}

// Bowls stacked, the top one with something left in it. A narrow foot under a
// wide body, so the stack steps out as bowls do; straight sides read as a tin.
static void bowls(Kit* kit, const KitFrame* f, float a, float d, float y, int count) {
    for (int i = 0; i < count; i++) {
        const float y0 = y + 0.028f * (float)i;
        kit_frame_prism(kit, f, MAT_CERAMIC, a, d, y0, y0 + 0.016f, 0.045f, 12);
        kit_frame_prism(kit, f, MAT_CERAMIC, a, d, y0 + 0.016f, y0 + 0.05f, 0.085f, 12);
    }
    const float top = y + 0.028f * (float)(count - 1) + 0.05f;
    kit_frame_prism(kit, f, MAT_CONTENTS, a, d, top - 0.004f, top + 0.001f, 0.075f, 12);
}

// A towel over the counter's front edge at d_edge: along the top, and down.
static void towel_over(Kit* kit, const KitFrame* f, float a0, float a1, float d_edge, float y) {
    kit_frame_box(kit, f, MAT_TOWEL, a0, a1, y, y + 0.012f, d_edge - 0.2f, d_edge + 0.012f, false);
    kit_frame_box(kit, f, MAT_TOWEL, a0, a1, y - 0.3f, y + 0.012f, d_edge, d_edge + 0.012f, false);
}

// Dish soap with its pump, and a bar on a dish beside it.
static void soap(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_prism(kit, f, MAT_PLASTIC, a, d, y, y + 0.15f, 0.03f, 8);
    kit_frame_prism(kit, f, MAT_BLACK, a, d, y + 0.15f, y + 0.19f, 0.009f, 6);
    kit_frame_box(kit, f, MAT_BLACK, a - 0.004f, a + 0.004f, y + 0.175f, y + 0.19f, d, d + 0.04f,
                  false);
    kit_frame_box(kit, f, MAT_CERAMIC, a + 0.06f, a + 0.18f, y, y + 0.012f, d - 0.04f, d + 0.04f,
                  false);
    kit_frame_box(kit, f, MAT_PAPER, a + 0.08f, a + 0.16f, y + 0.012f, y + 0.035f, d - 0.025f,
                  d + 0.025f, false);
}

/*
 * Clutter along a counter: `count` things from the vocabulary above, spaced
 * so none overlap, and never over [skip0, skip1] -- a sink, say.
 */
static void clutter(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1, float skip0,
                    float skip1, float y, int count) {
    float placed[16];
    int n = 0;
    for (int tries = 0; tries < 60 && n < count && n < 16; tries++) {
        const float a = kit_rrange(rng, a0, a1);
        if (a > skip0 && a < skip1)
            continue;
        bool clear = true;
        for (int i = 0; i < n; i++)
            if (fabsf(placed[i] - a) < 0.2f)
                clear = false;
        if (!clear)
            continue;
        placed[n++] = a;
        const float d = kit_rrange(rng, 0.15f, 0.42f);
        switch ((int)(kit_rnd(rng) * 6.0f)) {
            case 0:
                kitchen_plates(kit, f, a, d, y, 2 + (int)(kit_rnd(rng) * 4.0f));
                break;
            case 1:
                kitchen_mug(kit, f, a, d, y);
                break;
            case 2:
                bottle(kit, f, rng, a, d, y, kit_rrange(rng, 0.16f, 0.24f));
                break;
            case 3:
                jar(kit, f, rng, a, d, y, kit_rrange(rng, 0.04f, 0.06f),
                    kit_rrange(rng, 0.1f, 0.18f));
                break;
            case 4:
                pot(kit, f, a, d, y);
                break;
            default:
                kit_frame_box(kit, f, MAT_WOOD, a - 0.17f, a + 0.17f, y, y + 0.02f, d - 0.1f,
                              d + 0.12f, false);
                break;
        }
    }
}

// The window wall: counter with the sink under the window, uppers either side,
// the backsplash, and the window's own frame and sill.
static void window_wall(Kit* kit, KitRng* rng) {
    const KitFrame f = {{KITCHEN_X0, FLOOR_Y, KITCHEN_Z0}, 0.0f};
    const float len = KITCHEN_X1 - KITCHEN_X0;
    const float w0 = KITCHEN_WIN_X0 - KITCHEN_X0, w1 = KITCHEN_WIN_X1 - KITCHEN_X0;
    const float sill = KITCHEN_WIN_SILL - FLOOR_Y, head = KITCHEN_WIN_HEAD - FLOOR_Y;

    base_units(kit, &f, 0.0f, len, CARCASS_D, SINK_A0, SINK_A1, 0.70f);
    counter(kit, &f, MAT_TRIM, 0.0f, SINK_A0, 0.0f, COUNTER_D);
    counter(kit, &f, MAT_TRIM, SINK_A1, len, 0.0f, COUNTER_D);
    counter(kit, &f, MAT_TRIM, SINK_A0, SINK_A1, 0.0f, SINK_D0);
    counter(kit, &f, MAT_TRIM, SINK_A0, SINK_A1, SINK_D1, COUNTER_D);

    // The basin: a floor and four walls below the counter's opening, a drop-in
    // rim round the opening on the counter, and the drain.
    const float t = 0.015f, lip = 0.02f;
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0, SINK_A1, 0.70f, 0.72f, SINK_D0, SINK_D1, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0, SINK_A1, 0.72f, COUNTER_TOP, SINK_D0,
                  SINK_D0 + t, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0, SINK_A1, 0.72f, COUNTER_TOP, SINK_D1 - t,
                  SINK_D1, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0, SINK_A0 + t, 0.72f, COUNTER_TOP, SINK_D0,
                  SINK_D1, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A1 - t, SINK_A1, 0.72f, COUNTER_TOP, SINK_D0,
                  SINK_D1, false);
    const float rim = COUNTER_TOP + 0.004f;
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0 - lip, SINK_A1 + lip, COUNTER_TOP, rim,
                  SINK_D0 - lip, SINK_D0, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0 - lip, SINK_A1 + lip, COUNTER_TOP, rim, SINK_D1,
                  SINK_D1 + lip, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A0 - lip, SINK_A0, COUNTER_TOP, rim, SINK_D0,
                  SINK_D1, false);
    kit_frame_box(kit, &f, MAT_STAINLESS, SINK_A1, SINK_A1 + lip, COUNTER_TOP, rim, SINK_D0,
                  SINK_D1, false);
    const float am = 0.5f * (SINK_A0 + SINK_A1);
    kit_frame_prism(kit, &f, MAT_BLACK, am, 0.5f * (SINK_D0 + SINK_D1), 0.72f, 0.722f, 0.03f, 12);
    // Clear of the sill's front edge, which the riser would otherwise pass through.
    faucet(kit, &f, am, 0.065f, COUNTER_TOP);

    // Tile up the wall behind the counter, and under the sill.
    kit_frame_box(kit, &f, MAT_BACKSPLASH, 0.0f, w0 - 0.08f, COUNTER_TOP, UPPER_Y0, 0.0f, 0.012f,
                  false);
    kit_frame_box(kit, &f, MAT_BACKSPLASH, w1 + 0.08f, len, COUNTER_TOP, UPPER_Y0, 0.0f, 0.012f,
                  false);
    kit_frame_box(kit, &f, MAT_BACKSPLASH, w0 - 0.08f, w1 + 0.08f, COUNTER_TOP, sill - 0.02f, 0.0f,
                  0.012f, false);

    upper_units(kit, &f, 0.0f, w0 - 0.2f);
    upper_units(kit, &f, w1 + 0.2f, len);

    // The window: a heavy cross in the middle of the wall's thickness, a
    // casing round the opening on the room side, and a deep sill.
    const float wd0 = -0.5f * EXT_WALL - 0.03f, wd1 = -0.5f * EXT_WALL + 0.03f;
    const float bar = 0.06f, mid = 0.5f * (w0 + w1), cross = sill + 0.55f * (head - sill);
    kit_frame_box(kit, &f, MAT_TRIM, w0, w0 + bar, sill, head, wd0, wd1, false);
    kit_frame_box(kit, &f, MAT_TRIM, w1 - bar, w1, sill, head, wd0, wd1, false);
    kit_frame_box(kit, &f, MAT_TRIM, w0, w1, head - bar, head, wd0, wd1, false);
    kit_frame_box(kit, &f, MAT_TRIM, w0, w1, sill, sill + bar, wd0, wd1, false);
    kit_frame_box(kit, &f, MAT_TRIM, mid - 0.5f * bar, mid + 0.5f * bar, sill, head, wd0, wd1,
                  false);
    kit_frame_box(kit, &f, MAT_TRIM, w0, w1, cross - 0.5f * bar, cross + 0.5f * bar, wd0, wd1,
                  false);
    kit_frame_box(kit, &f, MAT_TRIM, w0 - 0.08f, w0, sill, head + 0.08f, 0.0f, 0.02f, false);
    kit_frame_box(kit, &f, MAT_TRIM, w1, w1 + 0.08f, sill, head + 0.08f, 0.0f, 0.02f, false);
    kit_frame_box(kit, &f, MAT_TRIM, w0 - 0.08f, w1 + 0.08f, head, head + 0.08f, 0.0f, 0.02f,
                  false);
    kit_frame_box(kit, &f, MAT_TRIM, w0 - 0.1f, w1 + 0.1f, sill - 0.02f, sill + 0.02f,
                  -0.5f * EXT_WALL, 0.05f, false);

    // The counter, laid out rather than scattered: knives and the utensils at
    // the back left, the board with a knife on it, bowls, a towel over the edge
    // by the sink, soap on the far side of it, plates drying, a mug, a pot.
    const float y = COUNTER_TOP;
    knife_block(kit, &f, 0.28f, 0.12f, y);
    utensil_can(kit, &f, 0.55f, 0.12f, y);
    cutting_board(kit, &f, 0.85f, 0.22f, y);
    bowls(kit, &f, 1.55f, 0.32f, y, 3);
    towel_over(kit, &f, 1.78f, 2.02f, COUNTER_D, y);
    soap(kit, &f, SINK_A1 + 0.1f, 0.1f, y);
    kitchen_plates(kit, &f, SINK_A1 + 0.5f, 0.32f, y, 4);
    kitchen_mug(kit, &f, 3.95f, 0.18f, y);
    pot(kit, &f, 4.35f, 0.3f, y);
    bottle(kit, &f, rng, 4.7f, 0.12f, y, 0.22f);

    // What gets put on top of the cupboards and forgotten: boxes, a pot, a
    // tin, bottles.
    const float top = UPPER_Y1;
    kit_frame_box(kit, &f, MAT_CARDBOARD, 0.04f, 0.46f, top, top + 0.2f, 0.02f, 0.31f, false);
    kit_frame_box(kit, &f, MAT_CARDBOARD, 0.1f, 0.38f, top + 0.2f, top + 0.31f, 0.05f, 0.27f,
                  false);
    pot(kit, &f, 0.66f, 0.17f, top);
    bottle(kit, &f, rng, 0.95f, 0.14f, top, 0.2f);
    kit_frame_prism(kit, &f, MAT_STEEL, 3.98f, 0.16f, top, top + 0.15f, 0.07f, 10);
    kit_frame_box(kit, &f, MAT_CARDBOARD, 4.15f, 4.68f, top, top + 0.25f, 0.03f, 0.3f, false);
    jar(kit, &f, rng, 4.78f, 0.16f, top, 0.05f, 0.16f);
}

static void fridge(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1);

// The hall wall's run, along -z from just short of the doorway to where the
// window run's counter begins; the fridge stands first in it.
static const KitFrame STOVE_WALL = {{KITCHEN_X0, FLOOR_Y, STOVE_RUN_Z}, 0.5f * GLM_PIf};
#define FRIDGE_W 0.72f

// The hall wall on the left, as you come in: the fridge, the stove under its
// hood, and a short run to the corner.
static void stove_wall(Kit* kit, KitRng* rng) {
    const float start = STOVE_RUN_Z;
    const KitFrame f = STOVE_WALL;
    const float len = start - (KITCHEN_Z0 + COUNTER_D);
    const float fridge_w = FRIDGE_W;
    const float s0 = start - STOVE_Z - 0.38f, s1 = start - STOVE_Z + 0.38f;

    fridge(kit, &f, rng, 0.0f, fridge_w);
    base_units(kit, &f, s1, len, CARCASS_D, 0.0f, 0.0f, 0.0f);
    counter(kit, &f, MAT_TRIM, s1, len, 0.0f, COUNTER_D);

    // The stove: body, cooktop, backguard, four burners, the oven door and
    // its bar, knobs, and a towel hung over the bar.
    kit_frame_box(kit, &f, MAT_APPLIANCE, s0, s1, 0.0f, 0.88f, 0.0f, 0.60f, true);
    kit_frame_box(kit, &f, MAT_BLACK, s0, s1, 0.88f, COUNTER_TOP, 0.0f, 0.60f, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, s0, s1, COUNTER_TOP, 1.03f, 0.0f, 0.06f, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? s1 - 0.2f : s0 + 0.2f;
        const float d = (i & 2) ? 0.44f : 0.22f;
        kit_frame_prism(kit, &f, MAT_BLACK, a, d, COUNTER_TOP, COUNTER_TOP + 0.015f, 0.085f, 8);
    }
    kit_frame_box(kit, &f, MAT_BLACK, s0 + 0.04f, s1 - 0.04f, 0.12f, 0.68f, 0.60f, 0.62f, false);
    kit_frame_bar(kit, &f, MAT_STEEL, s0 + 0.08f, s1 - 0.08f, 0.72f, 0.66f, 0.01f);
    // The knobs: a skirt on the panel, the knob, and a line to read it by.
    for (int i = 0; i < 4; i++) {
        const float a = s0 + 0.14f + 0.16f * (float)i, ky = 0.805f;
        kit_frame_pipe(kit, &f, MAT_STAINLESS, (vec3[]){{a, ky, 0.60f}, {a, ky, 0.606f}}, 2, 0.027f,
                       28);
        kit_frame_pipe(kit, &f, MAT_STAINLESS, (vec3[]){{a, ky, 0.60f}, {a, ky, 0.63f}}, 2, 0.021f,
                       28);
        kit_frame_box(kit, &f, MAT_BLACK, a - 0.002f, a + 0.002f, ky + 0.004f, ky + 0.019f, 0.63f,
                      0.6315f, false);
    }
    kit_frame_box(kit, &f, MAT_TOWEL, s0 + 0.2f, s0 + 0.44f, 0.36f, 0.73f, 0.672f, 0.68f, false);
    kit_frame_box(kit, &f, MAT_TOWEL, s0 + 0.2f, s0 + 0.44f, 0.71f, 0.745f, 0.64f, 0.68f, false);
    pot(kit, &f, s0 + 0.2f, 0.22f, COUNTER_TOP + 0.015f);
    pan(kit, &f, s1 - 0.2f, 0.44f, COUNTER_TOP + 0.015f);

    // The hood: a canopy, a shoulder, and the duct to the ceiling.
    kit_frame_box(kit, &f, MAT_STEEL, s0 - 0.04f, s1 + 0.04f, 1.62f, 1.74f, 0.0f, 0.5f, false);
    kit_frame_box(kit, &f, MAT_STEEL, s0 + 0.1f, s1 - 0.1f, 1.74f, 1.86f, 0.0f, 0.36f, false);
    kit_frame_box(kit, &f, MAT_STEEL, s0 + 0.22f, s1 - 0.22f, 1.86f, CEIL_Y - FLOOR_Y, 0.0f, 0.24f,
                  false);

    // Tile behind the stove and on to the window wall.
    kit_frame_box(kit, &f, MAT_BACKSPLASH, fridge_w, start - KITCHEN_Z0, COUNTER_TOP, UPPER_Y0,
                  0.0f, 0.012f, false);

    clutter(kit, &f, rng, s1 + 0.1f, len - 0.1f, 0.0f, 0.0f, COUNTER_TOP, 1);
}

// A card pinned upright facing out of its wall at d, centred at (a, y) and
// turned `tilt` radians in its own plane.
static void pin_card(Kit* kit, const KitFrame* f, CardId card, float a, float y, float d,
                     float tilt) {
    const float w = CARDS[card].size[0], h = CARDS[card].size[1];
    const float c = cosf(tilt), s = sinf(tilt);
    const vec3 across = {w * c, w * s, 0.0f}, up = {-h * s, h * c, 0.0f};
    const vec3 corner = {a - 0.5f * (across[0] + up[0]), y - 0.5f * (across[1] + up[1]), d};
    kit_frame_card(kit, f, MAT_CARDS, corner, across, up, CARDS[card].uv);
}

// A card lying face up at height y, centred at (a, d) and turned `turn`
// radians; its top is away from someone standing at +d.
static void lay_card(Kit* kit, const KitFrame* f, CardId card, float a, float d, float y,
                     float turn) {
    kit_frame_card_lying(kit, f, MAT_CARDS, CARDS[card].uv, CARDS[card].size[0],
                         CARDS[card].size[1], a, y, d, turn);
}

// A round fridge magnet on a surface facing out at d.
static void magnet(Kit* kit, const KitFrame* f, int mat, float a, float y, float d) {
    kit_frame_pipe(kit, f, mat, (vec3[]){{a, y, d}, {a, y, d + 0.008f}}, 2, 0.011f, 16);
}

/*
 * The fridge: a tall rounded box with its handles, and a bottle forgotten on
 * top. Snapshots on the freezer door, and on the door below a shopping list
 * held by a magnet with two more photos -- each a little crooked, and each a
 * hair further out than the last, so none fights the door or another card.
 */
static void fridge(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1) {
    const float d = 0.68f, h = 1.72f;
    kit_frame_box(kit, f, MAT_APPLIANCE, a0, a1, 0.0f, h, 0.0f, d, true);
    kit_frame_box(kit, f, MAT_APPLIANCE, a0 + 0.03f, a1 - 0.03f, h, h + 0.04f, 0.03f, d - 0.03f,
                  false);
    kit_frame_box(kit, f, MAT_BLACK, a0 + 0.01f, a1 - 0.01f, 1.2f, 1.215f, d, d + 0.004f, false);
    kit_frame_prism(kit, f, MAT_STEEL, a0 + 0.06f, d + 0.035f, 1.3f, 1.55f, 0.012f, 6);
    kit_frame_prism(kit, f, MAT_STEEL, a0 + 0.06f, d + 0.035f, 0.75f, 1.1f, 0.012f, 6);

    const float step = 0.0008f;
    pin_card(kit, f, CARD_PHOTO_TREE, a0 + 0.26f, 1.47f, d + step, -0.06f);
    pin_card(kit, f, CARD_PHOTO_LAKE, a0 + 0.45f, 1.53f, d + 2.0f * step, 0.04f);
    pin_card(kit, f, CARD_PHOTO_ROAD, a0 + 0.59f, 1.40f, d + 3.0f * step, -0.03f);
    magnet(kit, f, MAT_PLASTIC, a0 + 0.262f, 1.505f, d + 2.0f * step);
    magnet(kit, f, MAT_CERAMIC, a0 + 0.45f, 1.575f, d + 3.0f * step);
    magnet(kit, f, MAT_BLACK, a0 + 0.59f, 1.46f, d + 4.0f * step);

    pin_card(kit, f, CARD_PHOTO_PARK, a0 + 0.43f, 0.80f, d + step, 0.05f);
    pin_card(kit, f, CARD_PHOTO_GARDEN, a0 + 0.50f, 0.99f, d + 2.0f * step, -0.07f);
    pin_card(kit, f, CARD_NOTE, a0 + 0.30f, 1.03f, d + 3.0f * step, 0.03f);
    magnet(kit, f, MAT_PLASTIC, a0 + 0.298f, 1.085f, d + 4.0f * step);
    magnet(kit, f, MAT_BLACK, a0 + 0.44f, 1.03f, d + 3.0f * step);
    magnet(kit, f, MAT_CERAMIC, a0 + 0.43f, 0.835f, d + 2.0f * step);

    bottle(kit, f, rng, a0 + 0.2f, 0.3f, h + 0.04f, 0.2f);
    jar(kit, f, rng, a0 + 0.45f, 0.35f, h + 0.04f, 0.05f, 0.12f);
}

/*
 * A jar of homemade preserves, turned on the lathe so its glass bends light as
 * a round jar does: shouldered clear glass filled nearly to them with something
 * dark, under a screw cap or a paper or cloth cover tied round the neck, and a
 * paper label on most.
 */
static void preserve_jar(Kit* kit, const KitFrame* f, KitRng* rng, float a, float d, float y,
                         float r, float h) {
    const float neck = r - 0.008f;
    const vec2 shell[] = {{0.0f, 0.0f},   {r - 0.006f, 0.0f},          {r, 0.006f},
                          {r, h - 0.02f}, {neck + 0.002f, h - 0.008f}, {neck, h}};
    const int g = kit_rnd(rng) < 0.8f ? MAT_GLASS_CLEAR : MAT_GLASS_AMBER;
    kit_frame_lathe(kit, f, g, a, d, y, shell, COUNT(shell), 20);

    // Filled to below the shoulder, where the glass turns in past the fill.
    const float fill = fminf(h * kit_rrange(rng, 0.7f, 0.9f), h - 0.024f);
    const vec2 inside[] = {{0.0f, 0.004f},
                           {r - 0.005f, 0.004f},
                           {r - 0.004f, 0.01f},
                           {r - 0.004f, fill},
                           {0.0f, fill}};
    const float pick = kit_rnd(rng);
    const int fruit = pick < 0.4f ? MAT_JAM : (pick < 0.7f ? MAT_PLUM : MAT_PRUNE);
    kit_frame_lathe(kit, f, fruit, a, d, y, inside, COUNT(inside), 20);

    const float lid = kit_rnd(rng);
    if (lid < 0.6f) {
        const vec2 cap[] = {{neck + 0.001f, h - 0.006f},
                            {neck + 0.003f, h - 0.004f},
                            {neck + 0.003f, h + 0.01f},
                            {neck + 0.001f, h + 0.012f},
                            {0.0f, h + 0.012f}};
        kit_frame_lathe(kit, f, MAT_STEEL, a, d, y, cap, COUNT(cap), 16);
    } else {
        // A cover pulled down over the neck and tied, its skirt flaring out.
        const vec2 cover[] = {{neck + 0.012f, h - 0.022f},
                              {neck + 0.003f, h - 0.01f},
                              {neck + 0.004f, h + 0.002f},
                              {neck - 0.002f, h + 0.01f},
                              {0.0f, h + 0.012f}};
        const vec2 string[] = {{neck + 0.0045f, h - 0.013f}, {neck + 0.0045f, h - 0.009f}};
        kit_frame_lathe(kit, f, lid < 0.85f ? MAT_PAPER : MAT_TOWEL, a, d, y, cover, COUNT(cover),
                        16);
        kit_frame_lathe(kit, f, MAT_CARDBOARD, a, d, y, string, COUNT(string), 16);
    }
    if (kit_rnd(rng) < 0.45f) {
        const vec2 label[] = {{r + 0.0012f, h * 0.36f}, {r + 0.0012f, h * 0.58f}};
        kit_frame_lathe(kit, f, MAT_PAPER, a, d, y, label, COUNT(label), 20);
    }
}

void kitchen_preserves(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1, float y) {
    for (int row = 0; row < 2; row++) {
        const float d = row == 0 ? 0.075f : 0.185f;
        float a = a0 + (row == 0 ? 0.0f : 0.045f);
        for (;;) {
            const float r = kit_rrange(rng, 0.034f, 0.046f);
            if (a + 2.0f * r > a1)
                break;
            const float h =
                row == 0 ? kit_rrange(rng, 0.11f, 0.15f) : kit_rrange(rng, 0.08f, 0.12f);
            if (kit_rnd(rng) > 0.1f)
                preserve_jar(kit, f, rng, a + r, d, y, r, h);
            a += 2.0f * r + kit_rrange(rng, 0.004f, 0.03f);
        }
    }
}

// The outside wall on the right: a low sideboard with open shelves of
// preserves over it.
static void shelf_wall(Kit* kit, KitRng* rng) {
    const KitFrame f = {{KITCHEN_X1, FLOOR_Y, KITCHEN_Z0 + COUNTER_D}, -0.5f * GLM_PIf};
    const float s1 = 1.5f;
    base_units(kit, &f, 0.0f, s1, 0.45f, 0.0f, 0.0f, 0.0f);
    counter(kit, &f, MAT_WOOD, 0.0f, s1, 0.0f, 0.48f);

    const float boards[2] = {1.28f, 1.70f};
    for (int b = 0; b < 2; b++) {
        const float y = boards[b];
        kit_frame_box(kit, &f, MAT_WOOD, 0.05f, s1, y, y + 0.03f, 0.0f, 0.26f, false);
        kit_frame_box(kit, &f, MAT_STEEL, 0.15f, 0.18f, y - 0.12f, y, 0.0f, 0.2f, false);
        kit_frame_box(kit, &f, MAT_STEEL, s1 - 0.18f, s1 - 0.15f, y - 0.12f, y, 0.0f, 0.2f, false);
        kitchen_preserves(kit, &f, rng, 0.08f, s1 - 0.02f, y + 0.03f);
    }
    clutter(kit, &f, rng, 0.15f, s1 - 0.15f, 0.0f, 0.0f, COUNTER_TOP, 3);
}

void kitchen_chair(Kit* kit, const KitFrame* f) {
    const float s = 0.21f, leg = 0.018f;
    kit_frame_box(kit, f, MAT_WOOD, -s, s, 0.44f, 0.47f, -s, s, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? s - leg : -s + leg, d = (i & 2) ? s - leg : -s + leg;
        const float top = (i & 2) ? 0.44f : 0.98f; // the back legs run on up as posts
        kit_frame_box(kit, f, MAT_WOOD, a - leg, a + leg, 0.0f, top, d - leg, d + leg, false);
    }
    for (int r = 0; r < 3; r++) {
        const float y = 0.62f + 0.14f * (float)r;
        kit_frame_box(kit, f, MAT_WOOD, -s + leg, s - leg, y, y + 0.045f, -s + 0.005f,
                      -s + 2.0f * leg - 0.005f, false);
    }
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, -s, s, 0.0f, 0.47f, -s, s, true);
}

// One at (x, z) on the kitchen floor, facing along `yaw`.
static void chair(Kit* kit, float x, float z, float yaw) {
    kitchen_chair(kit, &(KitFrame){{x, FLOOR_Y, z}, yaw});
}

/*
 * A foam pad on a chair's seat, at (x, z) turned `yaw`. Four sides on the
 * lathe make it square, and the smooth normals round its sides over as a
 * cushion's are; the profile sags it a little in the middle, where it is sat
 * on. A lathe's first side lies along its frame's a, so the frame turns an
 * eighth more to square the pad with the seat.
 */
static void cushion(Kit* kit, float x, float z, float yaw) {
    static const vec2 PAD[] = {
        {0.0f, 0.0f},     {0.23f, 0.0f},   {0.2546f, 0.012f}, {0.252f, 0.03f},
        {0.225f, 0.043f}, {0.12f, 0.046f}, {0.0f, 0.042f},
    };
    const KitFrame f = {{x, FLOOR_Y, z}, yaw + 0.25f * GLM_PIf};
    kit_frame_lathe(kit, &f, MAT_CUSHION, 0.015f, -0.01f, 0.47f, PAD, COUNT(PAD), 4);
}

// The table and its two chairs, one pushed in and one pulled out askew.
static void table(Kit* kit) {
    const KitFrame f = {{3.2f, FLOOR_Y, 12.95f}, 0.0f};
    const float ha = 0.6f, hd = 0.4f, leg = 0.025f;
    kit_frame_box(kit, &f, MAT_TABLE, -ha, ha, 0.74f, 0.78f, -hd, hd, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? ha - 0.06f : -ha + 0.06f, d = (i & 2) ? hd - 0.06f : -hd + 0.06f;
        kit_frame_box(kit, &f, MAT_WOOD, a - leg, a + leg, 0.0f, 0.74f, d - leg, d + leg, false);
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -ha, ha, 0.0f, 0.78f, -hd, hd, true);

    kitchen_plates(kit, &f, -0.2f, -0.1f, 0.78f, 1);
    kitchen_mug(kit, &f, 0.25f, 0.15f, 0.78f);
    // The letter, read and left on a blank sheet it came with, and a snapshot
    // taken down off the fridge.
    const KitFrame sheet = {{3.46f, FLOOR_Y, 12.9f}, -0.2f};
    kit_frame_box(kit, &sheet, MAT_PAPER, -0.105f, 0.105f, 0.78f, 0.7808f, -0.1485f, 0.1485f,
                  false);
    // Turned half round to face the pulled-out chair, on the table's -d side.
    lay_card(kit, &f, CARD_LETTER, 0.2f, -0.08f, 0.7812f, GLM_PIf + 0.35f);
    lay_card(kit, &f, CARD_PHOTO_SNOW, -0.36f, 0.2f, 0.7806f, GLM_PIf - 0.5f);

    chair(kit, 2.38f, 12.95f, 0.5f * GLM_PIf);
    chair(kit, 3.35f, 12.12f, 0.35f);
    cushion(kit, 3.35f, 12.12f, 0.35f + 0.12f);
}

// Against the other loops tools/fetch_sounds.py levels alike.
#define FRIDGE_VOLUME 0.15f

void kitchen_start_audio(AudioSystem* audio) {
    Sound* hum = sounds_loop(audio, "assets/audio/silent/fridge_hum.flac");
    if (!hum)
        return;
    // Behind the fridge and near the floor, where the compressor sits.
    vec3 motor = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&STOVE_WALL, 0.5f * FRIDGE_W, 0.2f, 0.08f, motor);
    audio_sound_set_position(hum, motor);
    audio_sound_set_volume(hum, FRIDGE_VOLUME);
}

void kitchen_build(Kit* kit, unsigned int seed) {
    KitRng rng = {seed * 2654435761u + 12345u};
    window_wall(kit, &rng);
    stove_wall(kit, &rng);
    shelf_wall(kit, &rng);
    table(kit);
    kit_box(kit, MAT_RUG, STOVE_MAT, STOVE_MAT_HALF, 0.0f, false);
}

bool kitchen_on_mat(const vec3 p) {
    return fabsf(p[0] - STOVE_MAT[0]) < STOVE_MAT_HALF[0] &&
           fabsf(p[2] - STOVE_MAT[2]) < STOVE_MAT_HALF[2] && fabsf(p[1] - FLOOR_Y) < 0.05f;
}
