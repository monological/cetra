#include <math.h>

#include "fixtures.h"
#include "mats.h"

#define POST       0.006f  // half a cage post's square section
#define FRAME      0.012f  // a cage frame's height
#define PANE       0.002f  // half a pane's thickness
#define CROWN_SLAB 0.012f  // the crown's base slab's height, and its overhang past the cage
#define PYRAMID    0.055f  // the crown's roof
#define WIRE       0.0012f // the filament's
#define CAGE_R     0.009f  // the squirrel cage's radius
#define CAGE_H     0.011f  // its half height

void fixture_bulb(Kit* kit, const KitFrame* f, float a, float y, float d) {
    // A clear pear of glass, widest at the filament, its neck up into the socket.
    const vec2 glass[] = {{0.0f, -0.034f},   {0.01f, -0.032f}, {0.019f, -0.025f},
                          {0.026f, -0.012f}, {0.028f, 0.0f},   {0.026f, 0.014f},
                          {0.02f, 0.03f},    {0.014f, 0.045f}, {0.0125f, 0.058f}};
    kit_frame_lathe(kit, f, MAT_GLASS_CLEAR, a, d, y, glass, KIT_COUNT(glass), 18);
    const vec2 socket[] = {{0.0135f, 0.05f}, {0.0135f, 0.075f}, {0.0155f, 0.078f},
                           {0.0155f, 0.1f},  {0.011f, 0.108f},  {0.0f, 0.112f}};
    kit_frame_lathe(kit, f, MAT_LANTERN_BRASS, a, d, y, socket, KIT_COUNT(socket), 14);
    // The squirrel cage: one wire zigzagging round a ring between two, top and bottom, lit.
    vec3 zig[13];
    for (int i = 0; i < 13; i++) {
        const float t = (float)i * GLM_PIf / 6.0f;
        zig[i][0] = a + CAGE_R * cosf(t);
        zig[i][1] = y + ((i & 1) ? -CAGE_H : CAGE_H);
        zig[i][2] = d + CAGE_R * sinf(t);
    }
    kit_frame_pipe(kit, f, MAT_FILAMENT, zig, KIT_COUNT(zig), WIRE, 4);
    // Its two leads down from the socket.
    for (int s = -1; s <= 1; s += 2) {
        const vec3 lead[2] = {{a + 0.004f * (float)s, y + 0.052f, d},
                              {a + CAGE_R * (float)s, y + CAGE_H, d}};
        kit_frame_pipe(kit, f, MAT_BLACK, lead, 2, 0.0006f, 3);
    }
}

// A face of the crown's pyramid, from its base edge p0..p1 to its apex, facing `out`: a triangle,
// since a quad with the apex twice loses its normal when it is wound the other way round.
static void crown_face(Kit* kit, const KitFrame* f, const vec3 p0, const vec3 p1, const vec3 apex,
                       const vec3 out) {
    vec3 w0 = GLM_VEC3_ZERO_INIT, w1 = GLM_VEC3_ZERO_INIT, w2 = GLM_VEC3_ZERO_INIT;
    vec3 o = GLM_VEC3_ZERO_INIT;
    kit_frame_point(f, p0[0], p0[1], p0[2], w0);
    kit_frame_point(f, p1[0], p1[1], p1[2], w1);
    kit_frame_point(f, apex[0], apex[1], apex[2], w2);
    kit_frame_dir(f, out[0], out[1], out[2], o);
    kit_tri_facing(kit, MAT_LANTERN_BRASS, w0, w1, w2, o);
}

void fixture_lantern(Kit* kit, const KitFrame* f, float a, float y, float d, float r, float h) {
    const float top = y, foot = y - h;
    // The four posts, and the frames round the foot and the top.
    for (int i = 0; i < 4; i++) {
        const float pa = a + ((i & 1) ? r : -r), pd = d + ((i & 2) ? r : -r);
        kit_frame_box(kit, f, MAT_LANTERN_BRASS, pa - POST, pa + POST, foot, top, pd - POST,
                      pd + POST, false);
    }
    const float rows[2] = {foot, top - FRAME};
    for (int k = 0; k < 2; k++) {
        const float y0 = rows[k], y1 = rows[k] + FRAME;
        for (int s = -1; s <= 1; s += 2) {
            kit_frame_box(kit, f, MAT_LANTERN_BRASS, a - r - POST, a + r + POST, y0, y1,
                          d + (float)s * r - POST, d + (float)s * r + POST, false);
            kit_frame_box(kit, f, MAT_LANTERN_BRASS, a + (float)s * r - POST,
                          a + (float)s * r + POST, y0, y1, d - r + POST, d + r - POST, false);
        }
    }
    // The panes between the posts.
    for (int s = -1; s <= 1; s += 2) {
        kit_frame_box(kit, f, MAT_GLASS_CLEAR, a - r + POST, a + r - POST, foot + FRAME,
                      top - FRAME, d + (float)s * r - PANE, d + (float)s * r + PANE, false);
        kit_frame_box(kit, f, MAT_GLASS_CLEAR, a + (float)s * r - PANE, a + (float)s * r + PANE,
                      foot + FRAME, top - FRAME, d - r + POST, d + r - POST, false);
    }
    // Under the foot, a cross of rods and a drop finial where they meet.
    const float brace = foot + 0.5f * FRAME;
    for (int s = -1; s <= 1; s += 2) {
        const vec3 rod[2] = {{a - r, brace, d - (float)s * r}, {a + r, brace, d + (float)s * r}};
        kit_frame_pipe(kit, f, MAT_LANTERN_BRASS, rod, 2, 0.003f, 6);
    }
    const vec2 drop[] = {{0.0f, -0.045f},  {0.006f, -0.04f}, {0.011f, -0.025f},
                         {0.008f, -0.01f}, {0.004f, 0.0f},   {0.004f, 0.006f}};
    kit_frame_lathe(kit, f, MAT_LANTERN_BRASS, a, d, brace, drop, KIT_COUNT(drop), 12);
    // The crown: a slab over the cage, a pyramid on it, and a finial carrying a ring.
    const float rs = r + CROWN_SLAB, slab_top = top + CROWN_SLAB, rc = r + 0.008f;
    kit_frame_box(kit, f, MAT_LANTERN_BRASS, a - rs, a + rs, top, slab_top, d - rs, d + rs, false);
    const vec3 apex = {a, slab_top + PYRAMID, d};
    const vec3 c[4] = {{a - rc, slab_top, d - rc},
                       {a + rc, slab_top, d - rc},
                       {a + rc, slab_top, d + rc},
                       {a - rc, slab_top, d + rc}};
    crown_face(kit, f, c[0], c[1], apex, (vec3){0.0f, 0.6f, -1.0f});
    crown_face(kit, f, c[1], c[2], apex, (vec3){1.0f, 0.6f, 0.0f});
    crown_face(kit, f, c[2], c[3], apex, (vec3){0.0f, 0.6f, 1.0f});
    crown_face(kit, f, c[3], c[0], apex, (vec3){-1.0f, 0.6f, 0.0f});
    const vec2 finial[] = {{0.0f, -0.01f},   {0.01f, -0.004f}, {0.013f, 0.006f},
                           {0.009f, 0.016f}, {0.005f, 0.022f}, {0.0f, 0.025f}};
    kit_frame_lathe(kit, f, MAT_LANTERN_BRASS, a, d, apex[1], finial, KIT_COUNT(finial), 12);
    vec3 ring[13];
    const float ring_y = top + FIXTURE_CROWN_H - 0.012f;
    for (int i = 0; i < 13; i++) {
        const float t = (float)i * GLM_PIf / 6.0f;
        glm_vec3_copy((vec3){a + 0.012f * cosf(t), ring_y + 0.012f * sinf(t), d}, ring[i]);
    }
    kit_frame_pipe(kit, f, MAT_LANTERN_BRASS, ring, KIT_COUNT(ring), 0.0025f, 5);
    // The bulb, hung from the slab's middle on a short rod into its socket.
    const float bulb = top - FIXTURE_BULB_DROP;
    const vec3 rod[2] = {{a, bulb + 0.11f, d}, {a, top + 0.002f, d}};
    kit_frame_pipe(kit, f, MAT_LANTERN_BRASS, rod, 2, 0.004f, 8);
    fixture_bulb(kit, f, a, bulb, d);
}
