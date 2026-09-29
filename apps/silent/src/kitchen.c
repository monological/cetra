#include <math.h>
#include <stdint.h>

#include "kitchen.h"
#include "layout.h"
#include "mats.h"

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

// Repeatable clutter.
typedef struct Rng {
    uint32_t s;
} Rng;

static float rnd(Rng* r) {
    r->s = r->s * 1664525u + 1013904223u;
    return (float)(r->s >> 8) * (1.0f / 16777216.0f);
}

static float rrange(Rng* r, float lo, float hi) {
    return lo + (hi - lo) * rnd(r);
}

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

// Things on a surface at height y, in a frame: the clutter vocabulary.
static void plates(Kit* kit, const KitFrame* f, float a, float d, float y, int count) {
    for (int i = 0; i < count; i++)
        kit_frame_prism(kit, f, MAT_CERAMIC, a, d, y + 0.014f * (float)i,
                        y + 0.014f * (float)(i + 1) - 0.002f, 0.12f, 10);
}

static void mug(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_prism(kit, f, MAT_CERAMIC, a, d, y, y + 0.1f, 0.042f, 8);
}

/*
 * Containers are glass shells with what is in them standing inside: an opaque
 * fill to some level, which the refraction pass sees through the glass. Brown
 * glass mostly, as in any old pantry, and some clear.
 */
static int glass(Rng* rng) {
    return rnd(rng) < 0.6f ? MAT_GLASS_AMBER : MAT_GLASS_CLEAR;
}

static int contents(Rng* rng) {
    const float x = rnd(rng);
    return x < 0.45f ? MAT_CONTENTS : (x < 0.75f ? MAT_CONTENTS_PALE : MAT_CONTENTS_GREEN);
}

// Filled part way, so there is empty glass above what is in it for the wall
// to show through: that is most of what reads as glass.
static void bottle(Kit* kit, const KitFrame* f, Rng* rng, float a, float d, float y, float h) {
    const float r = 0.036f;
    const int g = glass(rng);
    kit_frame_prism(kit, f, g, a, d, y, y + h, r, 10);
    kit_frame_prism(kit, f, g, a, d, y + h, y + h + 0.08f, 0.013f, 8);
    kit_frame_prism(kit, f, MAT_CONTENTS, a, d, y + 0.004f, y + h * rrange(rng, 0.15f, 0.6f),
                    r - 0.005f, 10);
    kit_frame_prism(kit, f, MAT_BLACK, a, d, y + h + 0.08f, y + h + 0.095f, 0.015f, 8);
}

static void jar(Kit* kit, const KitFrame* f, Rng* rng, float a, float d, float y, float r,
                float h) {
    kit_frame_prism(kit, f, glass(rng), a, d, y, y + h, r, 12);
    kit_frame_prism(kit, f, contents(rng), a, d, y + 0.005f, y + h * rrange(rng, 0.25f, 0.7f),
                    r - 0.006f, 12);
    kit_frame_prism(kit, f, MAT_STEEL, a, d, y + h, y + h + 0.02f, r * 0.92f, 12);
}

static void pot(Kit* kit, const KitFrame* f, float a, float d, float y) {
    kit_frame_prism(kit, f, MAT_STEEL, a, d, y, y + 0.13f, 0.11f, 10);
}

/*
 * Clutter along a counter: `count` things from the vocabulary above, spaced
 * so none overlap, and never over [skip0, skip1] -- a sink, say.
 */
static void clutter(Kit* kit, const KitFrame* f, Rng* rng, float a0, float a1, float skip0,
                    float skip1, float y, int count) {
    float placed[16];
    int n = 0;
    for (int tries = 0; tries < 60 && n < count && n < 16; tries++) {
        const float a = rrange(rng, a0, a1);
        if (a > skip0 && a < skip1)
            continue;
        bool clear = true;
        for (int i = 0; i < n; i++)
            if (fabsf(placed[i] - a) < 0.2f)
                clear = false;
        if (!clear)
            continue;
        placed[n++] = a;
        const float d = rrange(rng, 0.15f, 0.42f);
        switch ((int)(rnd(rng) * 6.0f)) {
            case 0:
                plates(kit, f, a, d, y, 2 + (int)(rnd(rng) * 4.0f));
                break;
            case 1:
                mug(kit, f, a, d, y);
                break;
            case 2:
                bottle(kit, f, rng, a, d, y, rrange(rng, 0.16f, 0.24f));
                break;
            case 3:
                jar(kit, f, rng, a, d, y, rrange(rng, 0.04f, 0.06f), rrange(rng, 0.1f, 0.18f));
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
static void window_wall(Kit* kit, Rng* rng) {
    const KitFrame f = {{KITCHEN_X0, FLOOR_Y, KITCHEN_Z0}, 0.0f};
    const float len = KITCHEN_X1 - KITCHEN_X0;
    const float w0 = KITCHEN_WIN_X0 - KITCHEN_X0, w1 = KITCHEN_WIN_X1 - KITCHEN_X0;
    const float sill = KITCHEN_WIN_SILL - FLOOR_Y, head = KITCHEN_WIN_HEAD - FLOOR_Y;

    base_units(kit, &f, 0.0f, len, CARCASS_D, SINK_A0, SINK_A1, 0.70f);
    counter(kit, &f, MAT_TRIM, 0.0f, SINK_A0, 0.0f, COUNTER_D);
    counter(kit, &f, MAT_TRIM, SINK_A1, len, 0.0f, COUNTER_D);
    counter(kit, &f, MAT_TRIM, SINK_A0, SINK_A1, 0.0f, SINK_D0);
    counter(kit, &f, MAT_TRIM, SINK_A0, SINK_A1, SINK_D1, COUNTER_D);

    // The basin: a floor and four walls, below the counter's opening.
    const float t = 0.015f;
    kit_frame_box(kit, &f, MAT_STEEL, SINK_A0, SINK_A1, 0.70f, 0.72f, SINK_D0, SINK_D1, false);
    kit_frame_box(kit, &f, MAT_STEEL, SINK_A0, SINK_A1, 0.72f, COUNTER_TOP, SINK_D0, SINK_D0 + t,
                  false);
    kit_frame_box(kit, &f, MAT_STEEL, SINK_A0, SINK_A1, 0.72f, COUNTER_TOP, SINK_D1 - t, SINK_D1,
                  false);
    kit_frame_box(kit, &f, MAT_STEEL, SINK_A0, SINK_A0 + t, 0.72f, COUNTER_TOP, SINK_D0, SINK_D1,
                  false);
    kit_frame_box(kit, &f, MAT_STEEL, SINK_A1 - t, SINK_A1, 0.72f, COUNTER_TOP, SINK_D0, SINK_D1,
                  false);
    // The tap: a riser, a spout reaching over the basin, two handles.
    const float am = 0.5f * (SINK_A0 + SINK_A1);
    kit_frame_prism(kit, &f, MAT_STEEL, am, 0.05f, COUNTER_TOP, 1.18f, 0.013f, 8);
    kit_frame_box(kit, &f, MAT_STEEL, am - 0.012f, am + 0.012f, 1.155f, 1.18f, 0.05f, 0.24f, false);
    kit_frame_prism(kit, &f, MAT_STEEL, am - 0.11f, 0.05f, COUNTER_TOP, 0.97f, 0.018f, 6);
    kit_frame_prism(kit, &f, MAT_STEEL, am + 0.11f, 0.05f, COUNTER_TOP, 0.97f, 0.018f, 6);

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

    clutter(kit, &f, rng, 0.15f, len - 0.15f, SINK_A0 - 0.12f, SINK_A1 + 0.12f, COUNTER_TOP, 8);
    // Something on top of each run of uppers.
    bottle(kit, &f, rng, 0.3f, 0.15f, UPPER_Y1, 0.22f);
    jar(kit, &f, rng, len - 0.35f, 0.18f, UPPER_Y1, 0.07f, 0.2f);
}

static void fridge(Kit* kit, const KitFrame* f, Rng* rng, float a0, float a1);

// The hall wall on the left, as you come in: the fridge, the stove under its
// hood, and a short run to the corner.
static void stove_wall(Kit* kit, Rng* rng) {
    // Along -z from just short of the doorway to where the window run's
    // counter begins.
    const float start = STOVE_RUN_Z;
    const KitFrame f = {{KITCHEN_X0, FLOOR_Y, start}, 0.5f * GLM_PIf};
    const float len = start - (KITCHEN_Z0 + COUNTER_D);
    const float fridge_w = 0.72f;
    const float s0 = start - STOVE_Z - 0.38f, s1 = start - STOVE_Z + 0.38f;

    fridge(kit, &f, rng, 0.0f, fridge_w);
    base_units(kit, &f, s1, len, CARCASS_D, 0.0f, 0.0f, 0.0f);
    counter(kit, &f, MAT_TRIM, s1, len, 0.0f, COUNTER_D);

    // The stove: body, cooktop, backguard, four burners, the oven door and
    // its bar, knobs, and a towel hung over the bar.
    kit_frame_box(kit, &f, MAT_ENAMEL, s0, s1, 0.0f, 0.88f, 0.0f, 0.60f, true);
    kit_frame_box(kit, &f, MAT_BLACK, s0, s1, 0.88f, COUNTER_TOP, 0.0f, 0.60f, false);
    kit_frame_box(kit, &f, MAT_ENAMEL, s0, s1, COUNTER_TOP, 1.03f, 0.0f, 0.06f, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? s1 - 0.2f : s0 + 0.2f;
        const float d = (i & 2) ? 0.44f : 0.22f;
        kit_frame_prism(kit, &f, MAT_BLACK, a, d, COUNTER_TOP, COUNTER_TOP + 0.015f, 0.085f, 8);
    }
    kit_frame_box(kit, &f, MAT_BLACK, s0 + 0.04f, s1 - 0.04f, 0.12f, 0.68f, 0.60f, 0.62f, false);
    kit_frame_bar(kit, &f, MAT_STEEL, s0 + 0.08f, s1 - 0.08f, 0.72f, 0.66f, 0.01f);
    for (int i = 0; i < 4; i++) {
        const float a = s0 + 0.14f + 0.16f * (float)i;
        kit_frame_box(kit, &f, MAT_BLACK, a - 0.02f, a + 0.02f, 0.78f, 0.83f, 0.60f, 0.635f, false);
    }
    kit_frame_box(kit, &f, MAT_TOWEL, s0 + 0.2f, s0 + 0.44f, 0.36f, 0.73f, 0.672f, 0.68f, false);
    kit_frame_box(kit, &f, MAT_TOWEL, s0 + 0.2f, s0 + 0.44f, 0.71f, 0.745f, 0.64f, 0.68f, false);
    pot(kit, &f, s0 + 0.2f, 0.22f, COUNTER_TOP + 0.015f);

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

// The fridge: a tall rounded box with its handles, notes and photos, and a
// bottle forgotten on top.
static void fridge(Kit* kit, const KitFrame* f, Rng* rng, float a0, float a1) {
    const float d = 0.68f, h = 1.72f;
    kit_frame_box(kit, f, MAT_ENAMEL, a0, a1, 0.0f, h, 0.0f, d, true);
    kit_frame_box(kit, f, MAT_ENAMEL, a0 + 0.03f, a1 - 0.03f, h, h + 0.04f, 0.03f, d - 0.03f,
                  false);
    kit_frame_box(kit, f, MAT_BLACK, a0 + 0.01f, a1 - 0.01f, 1.2f, 1.215f, d, d + 0.004f, false);
    kit_frame_prism(kit, f, MAT_STEEL, a0 + 0.06f, d + 0.035f, 1.3f, 1.55f, 0.012f, 6);
    kit_frame_prism(kit, f, MAT_STEEL, a0 + 0.06f, d + 0.035f, 0.75f, 1.1f, 0.012f, 6);
    for (int i = 0; i < 5; i++) {
        const float w = rrange(rng, 0.09f, 0.18f), hh = rrange(rng, 0.08f, 0.22f);
        const float a = rrange(rng, a0 + 0.12f, a1 - 0.08f - w);
        const float y = rrange(rng, 0.5f, h - 0.1f - hh);
        const int mat = (i % 3 == 2) ? MAT_BLACK : MAT_PAPER;
        kit_frame_box(kit, f, mat, a, a + w, y, y + hh, d, d + 0.003f, false);
    }
    bottle(kit, f, rng, a0 + 0.2f, 0.3f, h + 0.04f, 0.2f);
    jar(kit, f, rng, a0 + 0.45f, 0.35f, h + 0.04f, 0.05f, 0.12f);
}

// The outside wall on the right: a low sideboard with open shelves of jars
// over it.
static void shelf_wall(Kit* kit, Rng* rng) {
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
        for (float a = 0.12f; a < s1 - 0.08f; a += rrange(rng, 0.11f, 0.2f)) {
            if (rnd(rng) < 0.7f)
                jar(kit, &f, rng, a, 0.13f, y + 0.03f, rrange(rng, 0.04f, 0.06f),
                    rrange(rng, 0.12f, 0.26f));
            else
                bottle(kit, &f, rng, a, 0.13f, y + 0.03f, rrange(rng, 0.18f, 0.26f));
        }
    }
    clutter(kit, &f, rng, 0.15f, s1 - 0.15f, 0.0f, 0.0f, COUNTER_TOP, 3);
}

// A ladder-back chair at (x, z), facing along `yaw` (its sitter faces +Z of
// that turn).
static void chair(Kit* kit, float x, float z, float yaw) {
    const KitFrame f = {{x, FLOOR_Y, z}, yaw};
    const float s = 0.21f, leg = 0.018f;
    kit_frame_box(kit, &f, MAT_WOOD, -s, s, 0.44f, 0.47f, -s, s, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? s - leg : -s + leg, d = (i & 2) ? s - leg : -s + leg;
        const float top = (i & 2) ? 0.44f : 0.98f; // the back legs run on up as posts
        kit_frame_box(kit, &f, MAT_WOOD, a - leg, a + leg, 0.0f, top, d - leg, d + leg, false);
    }
    for (int r = 0; r < 3; r++) {
        const float y = 0.62f + 0.14f * (float)r;
        kit_frame_box(kit, &f, MAT_WOOD, -s + leg, s - leg, y, y + 0.045f, -s + 0.005f,
                      -s + 2.0f * leg - 0.005f, false);
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -s, s, 0.0f, 0.47f, -s, s, true);
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

    plates(kit, &f, -0.2f, -0.1f, 0.78f, 1);
    mug(kit, &f, 0.25f, 0.15f, 0.78f);
    const KitFrame note = {{3.55f, FLOOR_Y, 12.85f}, 0.45f};
    kit_frame_box(kit, &note, MAT_PAPER, -0.1f, 0.1f, 0.78f, 0.782f, -0.14f, 0.14f, false);

    chair(kit, 2.38f, 12.95f, 0.5f * GLM_PIf);
    chair(kit, 3.35f, 12.12f, 0.35f);
}

void kitchen_build(Kit* kit, unsigned int seed) {
    Rng rng = {seed * 2654435761u + 12345u};
    window_wall(kit, &rng);
    stove_wall(kit, &rng);
    shelf_wall(kit, &rng);
    table(kit);
    // The mat in front of the stove.
    kit_box(kit, MAT_RUG, (vec3){1.12f, FLOOR_Y + 0.004f, 11.72f}, (vec3){0.38f, 0.004f, 0.62f},
            0.0f, false);
}
