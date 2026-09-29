#include <math.h>

#include "houses.h"
#include "mats.h"

#define FLOOR_RISE 0.45f // foundation: the ground floor sits this far up
#define STOREY_H   2.7f

// A local direction taken into the world: the frame's yaw without its origin.
static void frame_dir(const KitFrame* f, float a, float y, float d, vec3 out) {
    vec3 o = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.0f, 0.0f, 0.0f, o);
    kit_frame_point(f, a, y, d, out);
    glm_vec3_sub(out, o, out);
}

// One roof slope from the eave (d_eave) up to the ridge (d_ridge): the
// shingled top and, a roof's thickness under it, the soffit you see from the
// street under the overhang.
static void slope(Kit* kit, const KitFrame* f, float a0, float a1, float d_eave, float y_eave,
                  float d_ridge, float y_ridge, float toward) {
    const float t = 0.1f;
    vec3 p[4] = {{0.0f}}, q[4] = {{0.0f}}, up = {0.0f, 0.0f, 0.0f}, down = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a0, y_eave, d_eave, p[0]);
    kit_frame_point(f, a1, y_eave, d_eave, p[1]);
    kit_frame_point(f, a1, y_ridge, d_ridge, p[2]);
    kit_frame_point(f, a0, y_ridge, d_ridge, p[3]);
    for (int i = 0; i < 4; i++) {
        glm_vec3_copy(p[i], q[i]);
        q[i][1] -= t;
    }
    // Outward is up and toward the eave the slope falls to.
    const float run = fabsf(d_ridge - d_eave), rise = y_ridge - y_eave;
    frame_dir(f, 0.0f, run, toward * rise, up);
    glm_vec3_negate_to(up, down);
    kit_quad_facing(kit, MAT_ROOF, p[0], p[1], p[2], p[3], up);
    kit_quad_facing(kit, MAT_TRIM, q[0], q[1], q[2], q[3], down);
    // The fascia board along the eave's edge.
    vec3 edge = {0.0f, 0.0f, 0.0f};
    frame_dir(f, 0.0f, 0.0f, toward, edge);
    kit_quad_facing(kit, MAT_TRIM, p[0], p[1], q[1], q[0], edge);
}

void house_gable_roof(Kit* kit, const KitFrame* f, float w, float depth, float eave_y, float rise,
                      float overhang, int mat_gable) {
    const float slope_k = rise / (0.5f * depth);
    const float y_tip = eave_y - slope_k * overhang; // the eave, carried out to the overhang
    const float y_ridge = eave_y + rise;
    const float d_mid = -0.5f * depth;
    slope(kit, f, -overhang, w + overhang, overhang, y_tip, d_mid, y_ridge, 1.0f);
    slope(kit, f, -overhang, w + overhang, -depth - overhang, y_tip, d_mid, y_ridge, -1.0f);

    // The gable ends close the attic under the ridge.
    for (int side = 0; side < 2; side++) {
        const float a = side ? w : 0.0f;
        vec3 p0 = {0.0f, 0.0f, 0.0f}, p1 = {0.0f, 0.0f, 0.0f}, p2 = {0.0f, 0.0f, 0.0f};
        vec3 out = {0.0f, 0.0f, 0.0f};
        kit_frame_point(f, a, eave_y, 0.0f, p0);
        kit_frame_point(f, a, eave_y, -depth, p1);
        kit_frame_point(f, a, y_ridge, d_mid, p2);
        frame_dir(f, side ? 1.0f : -1.0f, 0.0f, 0.0f, out);
        kit_tri_facing(kit, mat_gable, p0, p1, p2, out);
    }
}

// One window on the facade: a pane (lit or dark), a blind sometimes, the
// cross of the sash, and a frame with a sill.
static void window(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1, float y0, float y1,
                   bool night) {
    const bool lit = night && kit_rnd(rng) < 0.28f;
    const bool boarded = !lit && kit_rnd(rng) < 0.08f;
    kit_frame_box(kit, f, lit ? MAT_WINDOW_LIT : MAT_DARK_GLASS, a0, a1, y0, y1, 0.0f, 0.02f,
                  false);
    if (boarded) {
        for (int i = 0; i < 3; i++) {
            const float y = y0 + (y1 - y0) * (0.2f + 0.3f * (float)i);
            kit_frame_box(kit, f, MAT_PORCH, a0 - 0.05f, a1 + 0.05f, y - 0.08f, y + 0.08f, 0.07f,
                          0.1f, false);
        }
    } else if (kit_rnd(rng) < 0.5f) {
        const float drop = kit_rrange(rng, 0.15f, 0.7f) * (y1 - y0);
        kit_frame_box(kit, f, MAT_PAPER, a0, a1, y1 - drop, y1, 0.02f, 0.03f, false);
    }
    const float mid = 0.5f * (a0 + a1), cross = y0 + 0.55f * (y1 - y0), b = 0.025f;
    kit_frame_box(kit, f, MAT_TRIM, mid - b, mid + b, y0, y1, 0.02f, 0.05f, false);
    kit_frame_box(kit, f, MAT_TRIM, a0, a1, cross - b, cross + b, 0.02f, 0.05f, false);
    kit_frame_box(kit, f, MAT_TRIM, a0 - 0.08f, a0, y0 - 0.08f, y1 + 0.08f, 0.0f, 0.07f, false);
    kit_frame_box(kit, f, MAT_TRIM, a1, a1 + 0.08f, y0 - 0.08f, y1 + 0.08f, 0.0f, 0.07f, false);
    kit_frame_box(kit, f, MAT_TRIM, a0 - 0.08f, a1 + 0.08f, y1, y1 + 0.1f, 0.0f, 0.07f, false);
    kit_frame_box(kit, f, MAT_TRIM, a0 - 0.12f, a1 + 0.12f, y0 - 0.1f, y0, 0.0f, 0.12f, false);
}

void house_neighbour(Kit* kit, const KitFrame* f, KitRng* rng, bool night) {
    // The frame arrives with its origin at the lot's front centre; the house
    // is measured from its own corner, so shift along the facade by half.
    const float w = kit_rrange(rng, 7.5f, 10.0f);
    const float depth = kit_rrange(rng, 6.5f, 8.5f);
    KitFrame h = *f;
    kit_frame_point(f, -0.5f * w, 0.0f, 0.0f, h.origin);

    const int storeys = kit_rnd(rng) < 0.55f ? 2 : 1;
    const float eave = FLOOR_RISE + (float)storeys * STOREY_H;
    const int sidings[3] = {MAT_SIDING, MAT_SIDING_B, MAT_SIDING_C};
    const int siding = sidings[(int)(kit_rnd(rng) * 3.0f) % 3];

    kit_frame_box(kit, &h, MAT_BRICK, -0.06f, w + 0.06f, 0.0f, FLOOR_RISE, -depth - 0.06f, 0.06f,
                  false);
    kit_frame_box(kit, &h, siding, 0.0f, w, FLOOR_RISE, eave, -depth, 0.0f, true);
    house_gable_roof(kit, &h, w, depth, eave, 0.36f * depth, 0.45f, siding);
    if (kit_rnd(rng) < 0.6f) {
        const float c = kit_rrange(rng, 1.0f, w - 1.7f);
        kit_frame_box(kit, &h, MAT_BRICK, c, c + 0.7f, eave, eave + 0.36f * depth + 0.9f,
                      -0.55f * depth, -0.55f * depth + 0.7f, false);
    }

    // The door, and a porch over it or a step up to it.
    const float door = kit_rrange(rng, 1.2f, w - 2.2f);
    kit_frame_box(kit, &h, MAT_WOOD, door, door + 0.95f, FLOOR_RISE, FLOOR_RISE + 2.1f, 0.0f, 0.05f,
                  false);
    kit_frame_box(kit, &h, MAT_TRIM, door - 0.1f, door, FLOOR_RISE, FLOOR_RISE + 2.2f, 0.0f, 0.08f,
                  false);
    kit_frame_box(kit, &h, MAT_TRIM, door + 0.95f, door + 1.05f, FLOOR_RISE, FLOOR_RISE + 2.2f,
                  0.0f, 0.08f, false);
    kit_frame_box(kit, &h, MAT_TRIM, door - 0.1f, door + 1.05f, FLOOR_RISE + 2.1f,
                  FLOOR_RISE + 2.2f, 0.0f, 0.08f, false);
    if (kit_rnd(rng) < 0.7f) {
        const float p0 = door - 1.0f, p1 = door + 1.95f;
        kit_frame_box(kit, &h, MAT_PORCH, p0, p1, 0.0f, FLOOR_RISE, 0.0f, 1.8f, true);
        kit_frame_box(kit, &h, MAT_ROOF, p0 - 0.1f, p1 + 0.1f, FLOOR_RISE + 2.55f,
                      FLOOR_RISE + 2.7f, 0.0f, 1.95f, false);
        kit_frame_prism(kit, &h, MAT_TRIM, p0 + 0.1f, 1.7f, FLOOR_RISE, FLOOR_RISE + 2.55f, 0.06f,
                        6);
        kit_frame_prism(kit, &h, MAT_TRIM, p1 - 0.1f, 1.7f, FLOOR_RISE, FLOOR_RISE + 2.55f, 0.06f,
                        6);
    } else {
        kit_frame_box(kit, &h, MAT_CONCRETE, door - 0.2f, door + 1.15f, 0.0f, 0.5f * FLOOR_RISE,
                      0.0f, 0.6f, true);
    }

    // Windows in a row per storey, clear of the door.
    const int slots = (int)fmaxf(2.0f, floorf((w - 0.8f) / 2.0f));
    const float pitch = (w - 0.8f) / (float)slots;
    for (int s = 0; s < storeys; s++) {
        const float y0 = FLOOR_RISE + (float)s * STOREY_H + 0.85f;
        for (int i = 0; i < slots; i++) {
            const float c = 0.4f + ((float)i + 0.5f) * pitch;
            if (s == 0 && fabsf(c - (door + 0.475f)) < 1.3f)
                continue;
            window(kit, &h, rng, c - 0.5f, c + 0.5f, y0, y0 + 1.3f, night);
        }
    }
}
