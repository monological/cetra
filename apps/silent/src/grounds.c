#include <math.h>

#include "grounds.h"
#include "hill.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "street.h"

/*
 * The mansion's front gate and its fence, the drive's dead lamps and the graveyard beside it
 * (spec 13.25). Rust, rubble and stone, all of it the street's own materials: the house up the
 * hill belongs to the same town gone further to ruin.
 */

#define FENCE_HEIGHT  1.8f
#define FENCE_SPACING 0.18f // between bars
#define FENCE_BAR     0.014f
#define FENCE_INSET   1.0f // in from the grounds' edge
#define GATE_HALF     2.0f // half the opening between the piers

// The gate is in line with the house's front path.
#define GATE_X (MANSION_X + 0.5f * (PATH_X0 + PATH_X1))
#define GATE_Z (GROUNDS_Z0 + FENCE_INSET)

static float rnd(unsigned int* state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return (float)(*state & 0xffffffu) / 16777215.0f;
}

/*
 * A box turned by `yaw` about the vertical and then leaning `lean` about its own X, so a
 * headstone or a cross stands askew the way the ground has let it fall. Six faces wound outward;
 * no collider, since what leans here is knee high or less.
 */
static void leaning_box(Kit* kit, int mat, const vec3 base, const vec3 half, float yaw,
                        float lean) {
    vec3 corner[8];
    for (int i = 0; i < 8; i++) {
        vec3 p = {(i & 1) ? half[0] : -half[0], (i & 2) ? 2.0f * half[1] : 0.0f,
                  (i & 4) ? half[2] : -half[2]};
        // Lean about X, then turn about Y, then stand on the base.
        const float y = p[1] * cosf(lean) - p[2] * sinf(lean);
        const float z = p[1] * sinf(lean) + p[2] * cosf(lean);
        corner[i][0] = base[0] + p[0] * cosf(yaw) + z * sinf(yaw);
        corner[i][1] = base[1] + y;
        corner[i][2] = base[2] - p[0] * sinf(yaw) + z * cosf(yaw);
    }
    vec3 centre = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; i++)
        glm_vec3_add(centre, corner[i], centre);
    glm_vec3_scale(centre, 1.0f / 8.0f, centre);
    static const int FACES[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                    {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) {
        vec3 mid = {0.0f, 0.0f, 0.0f}, out;
        for (int k = 0; k < 4; k++)
            glm_vec3_add(mid, corner[FACES[f][k]], mid);
        glm_vec3_scale(mid, 0.25f, mid);
        glm_vec3_sub(mid, centre, out);
        kit_quad_facing(kit, mat, corner[FACES[f][0]], corner[FACES[f][1]], corner[FACES[f][2]],
                        corner[FACES[f][3]], out);
    }
}

// A run of railing from (x0, z0) to (x1, z1) on ground level `y`: bars with a spike each, two
// rails, and a collider the length of it. `gaps` holds `gap_count` spans, in metres along the
// run, where bars have fallen or been taken.
static void railing(Kit* kit, float x0, float z0, float x1, float z1, float y, const float* gaps,
                    int gap_count) {
    const float len = hypotf(x1 - x0, z1 - z0);
    const float dx = (x1 - x0) / len, dz = (z1 - z0) / len;
    const float yaw = atan2f(dx, dz);
    for (float s = 0.5f * FENCE_SPACING; s < len; s += FENCE_SPACING) {
        bool gone = false;
        for (int g = 0; g < gap_count; g++)
            gone = gone || (s > gaps[2 * g] && s < gaps[2 * g + 1]);
        if (gone)
            continue;
        const float x = x0 + dx * s, z = z0 + dz * s;
        kit_box(kit, MAT_LAMP_POST, (vec3){x, y + 0.5f * FENCE_HEIGHT, z},
                (vec3){FENCE_BAR, 0.5f * FENCE_HEIGHT, FENCE_BAR}, yaw, false);
        // The spear's head, a diamond turned edge on.
        kit_box(kit, MAT_LAMP_POST, (vec3){x, y + FENCE_HEIGHT + 0.05f, z},
                (vec3){0.03f, 0.05f, 0.008f}, yaw + 0.785f, false);
    }
    // The rails run through the gaps too, bent and sagging there in the eye's account of it.
    for (int r = 0; r < 2; r++) {
        const float ry = y + (r ? FENCE_HEIGHT - 0.12f : 0.25f);
        kit_box(kit, MAT_LAMP_POST, (vec3){0.5f * (x0 + x1), ry, 0.5f * (z0 + z1)},
                (vec3){0.012f, 0.02f, 0.5f * len}, yaw, false);
    }
    kit_collider(kit, (vec3){0.5f * (x0 + x1), y + 1.0f, 0.5f * (z0 + z1)},
                 (vec3){0.05f, 1.0f, 0.5f * len}, yaw);
}

// A gate leaf hinged at (hx, hz), `width` long along `yaw`: a frame of flat bar and bars inside
// it, spiked like the fence.
static void gate_leaf(Kit* kit, float hx, float hz, float y, float width, float yaw) {
    const float dx = sinf(yaw), dz = cosf(yaw);
    for (float s = 0.1f; s < width; s += FENCE_SPACING) {
        const float x = hx + dx * s, z = hz + dz * s;
        kit_box(kit, MAT_LAMP_POST, (vec3){x, y + 1.05f, z}, (vec3){FENCE_BAR, 0.95f, FENCE_BAR},
                yaw, false);
        kit_box(kit, MAT_LAMP_POST, (vec3){x, y + 2.05f, z}, (vec3){0.03f, 0.05f, 0.008f},
                yaw + 0.785f, false);
    }
    for (int r = 0; r < 3; r++) {
        const float ry = y + 0.15f + 0.85f * (float)r;
        kit_box(kit, MAT_LAMP_POST, (vec3){hx + dx * 0.5f * width, ry, hz + dz * 0.5f * width},
                (vec3){0.02f, 0.03f, 0.5f * width}, yaw, false);
    }
    kit_collider(kit, (vec3){hx + dx * 0.5f * width, y + 1.0f, hz + dz * 0.5f * width},
                 (vec3){0.05f, 1.0f, 0.5f * width}, yaw);
}

static void pier(Kit* kit, float x, float z, float y) {
    kit_box(kit, MAT_FOUNDATION, (vec3){x, y + 1.2f, z}, (vec3){0.35f, 1.2f, 0.35f}, 0.0f, true);
    kit_box(kit, MAT_STONE, (vec3){x, y + 2.46f, z}, (vec3){0.43f, 0.06f, 0.43f}, 0.0f, false);
    kit_prism(kit, MAT_STONE, x, z, y + 2.52f, y + 2.8f, 0.16f, 8, false);
}

static void gate_and_fence(Kit* kit, unsigned int* rng) {
    const float y = MANSION_Y;
    const float x0 = GROUNDS_X0 + FENCE_INSET, x1 = GROUNDS_X1 - FENCE_INSET;
    const float z0 = GATE_Z, z1 = GROUNDS_Z1 - FENCE_INSET;
    pier(kit, GATE_X - GATE_HALF - 0.35f, z0, y);
    pier(kit, GATE_X + GATE_HALF + 0.35f, z0, y);
    // The east leaf shut, the west one hanging open into the grounds.
    gate_leaf(kit, GATE_X + GATE_HALF, z0, y, GATE_HALF, -0.5f * GLM_PIf);
    gate_leaf(kit, GATE_X - GATE_HALF, z0, y, GATE_HALF, 0.35f * GLM_PIf);

    // A gap or two a side, where the bars have gone.
    float gaps[2];
    const float run_x = x1 - x0, run_z = z1 - z0;
    gaps[0] = 4.0f + rnd(rng) * (run_z - 10.0f);
    gaps[1] = gaps[0] + 1.5f + rnd(rng) * 1.5f;
    railing(kit, x0, z0, x0, z1, y, gaps, 1);
    gaps[0] = 4.0f + rnd(rng) * (run_z - 10.0f);
    gaps[1] = gaps[0] + 1.5f + rnd(rng) * 1.5f;
    railing(kit, x1, z0, x1, z1, y, gaps, 1);
    gaps[0] = 4.0f + rnd(rng) * (run_x - 10.0f);
    gaps[1] = gaps[0] + 2.0f + rnd(rng) * 2.0f;
    railing(kit, x0, z1, x1, z1, y, gaps, 1);
    railing(kit, x0, z0, GATE_X - GATE_HALF - 0.7f, z0, y, NULL, 0);
    railing(kit, GATE_X + GATE_HALF + 0.7f, z0, x1, z0, y, NULL, 0);

    // The path up to the porch steps, as the house on the street has.
    const float path_z0 = z0 + 0.5f, path_z1 = MANSION_Z + PORCH_Z0 - 0.32f;
    kit_box(kit, MAT_CONCRETE,
            (vec3){MANSION_X + 0.5f * (PATH_X0 + PATH_X1), y + 0.01f, 0.5f * (path_z0 + path_z1)},
            (vec3){0.5f * (PATH_X1 - PATH_X0), 0.01f, 0.5f * (path_z1 - path_z0)}, 0.0f, false);
}

// The graveyard: a low rubble wall round a plot on the slope beside the drive's switchback, its
// way in facing the drive, and the stones inside leaning every way the ground has let them.
#define YARD_X  70.0f
#define YARD_Z  42.0f
#define YARD_HX 4.5f
#define YARD_HZ 3.5f

static void graveyard(Kit* kit, unsigned int* rng) {
    // The wall in short lengths, each on the ground under it.
    const float step = 1.0f;
    const struct {
        float ax, az, bx, bz;
    } sides[] = {{-YARD_HX, -YARD_HZ, YARD_HX, -YARD_HZ},
                 {YARD_HX, -YARD_HZ, YARD_HX, YARD_HZ},
                 {YARD_HX, YARD_HZ, -YARD_HX, YARD_HZ},
                 {-YARD_HX, YARD_HZ, -YARD_HX, -YARD_HZ}};
    for (int s = 0; s < 4; s++) {
        const float len = hypotf(sides[s].bx - sides[s].ax, sides[s].bz - sides[s].az);
        const float yaw = atan2f(sides[s].bx - sides[s].ax, sides[s].bz - sides[s].az);
        for (float t = 0.0f; t < len - 0.01f; t += step) {
            // The way in, on the side toward the drive.
            if (s == 1 && t > 2.5f && t < 4.5f)
                continue;
            const float m = (t + 0.5f * step) / len;
            const float x = YARD_X + sides[s].ax + (sides[s].bx - sides[s].ax) * m;
            const float z = YARD_Z + sides[s].az + (sides[s].bz - sides[s].az) * m;
            const float y = land_height(x, z);
            kit_box(kit, MAT_FOUNDATION, (vec3){x, y + 0.25f, z},
                    (vec3){0.2f, 0.45f + 0.1f * rnd(rng), 0.5f * step + 0.02f}, yaw, true);
        }
    }
    // Two rows of stones, one of them a cross.
    for (int i = 0; i < 9; i++) {
        const int column = i % 5, row = i / 5;
        const float x = YARD_X - YARD_HX + 1.2f + (float)column * 1.6f + 0.3f * rnd(rng);
        const float z = YARD_Z - 1.4f + (float)row * 2.4f + 0.3f * rnd(rng);
        const vec3 base = {x, land_height(x, z) - 0.15f, z};
        const float yaw = 0.15f * (rnd(rng) - 0.5f);
        const float lean = 0.35f * (rnd(rng) - 0.5f);
        if (i % 4 == 3) {
            leaning_box(kit, MAT_STONE, base, (vec3){0.06f, 0.6f, 0.06f}, yaw, lean);
            // Up the leaning shaft to where the arm crosses it.
            const vec3 arm = {base[0] + 0.8f * sinf(lean) * sinf(yaw), base[1] + 0.8f * cosf(lean),
                              base[2] + 0.8f * sinf(lean) * cosf(yaw)};
            leaning_box(kit, MAT_STONE, arm, (vec3){0.3f, 0.05f, 0.05f}, yaw, lean);
        } else {
            leaning_box(kit, MAT_STONE, base,
                        (vec3){0.25f + 0.08f * rnd(rng), 0.35f + 0.2f * rnd(rng), 0.06f}, yaw,
                        lean);
        }
    }
}

void grounds_build(Grounds* grounds, Kit* kit, Scene* scene, unsigned int seed, bool night) {
    *grounds = (Grounds){0};
    unsigned int rng = seed * 747796405u + 2891336453u;
    gate_and_fence(kit, &rng);
    graveyard(kit, &rng);

    // Two lamps up the drive, beside it with their arms out over it: the lower one dead, the
    // upper one failing.
    const int profile = street_lamp_profile(scene, night);
    const float at[2] = {0.32f, 0.72f};
    for (int i = 0; i < 2; i++) {
        float x, z, dx, dz;
        hill_drive_frame(at[i], &x, &z, &dx, &dz);
        // To the drive's left, the arm reaching back across it.
        const float lx = x - dz * 3.6f, lz = z + dx * 3.6f;
        const float yaw = atan2f(dz, -dx);
        Light* light =
            street_lamp(kit, scene, lx, land_height(lx, lz), lz, yaw, night, i == 0, profile);
        if (light) {
            grounds->failing = light;
            grounds->base_intensity = light->intensity;
        }
    }
}

void grounds_update(Grounds* grounds, double time) {
    if (!grounds->failing)
        return;
    // A failing ballast: out for a beat now and then, and buzzing between, in a pattern hashed
    // from the time so it never settles into a rhythm.
    const unsigned int beat = (unsigned int)(time * 9.0);
    unsigned int h = beat * 2654435761u;
    h ^= h >> 15;
    const unsigned int stretch = (unsigned int)(time * 0.6);
    unsigned int s = stretch * 2246822519u;
    s ^= s >> 13;
    const bool out = (h & 0xffu) < 70u || (s & 0xffu) < 50u;
    grounds->failing->intensity = out ? 0.04f * grounds->base_intensity : grounds->base_intensity;
}
