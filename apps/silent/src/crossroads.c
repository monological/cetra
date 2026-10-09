#include <math.h>

#include "cards.h"
#include "crossroads.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "street.h"

/*
 * The crossroads (spec 13.35). Its ground is boxes, as the street's is -- the street's road and
 * sidewalks carried on west, the cross street across them, and the corners -- out to CROSS_X0,
 * where the land's grid takes over for the last ragged strip before the lip. Across the road's
 * end that strip is not there: the road stops square at CROSS_X0 and what is past it has gone over
 * the edge.
 *
 * Every barricade is one row of BARRICADES and one body across from the lip to whatever closes
 * the arm's other side, so a later mechanic can take one away.
 */

#define CROSS_HALF 4.0f              // the cross street's asphalt, either side of its centre line
#define WALK_WIDTH 2.0f              // its sidewalks
#define RAIL_X     (CROSS_X0 + 1.0f) // the guard rail across the road's end

typedef struct Barricade {
    float z;      // its line across the arm
    float east_x; // where the arm's east side is closed already: the terrace's wall, our fence
    bool police;  // a police car left across the road behind it
    float behind; // which way along z is behind it, away from the crossroads
    // A way through it, across the road, or none when they are equal: its sawhorses pulled to
    // either side and the one with the sign dragged off the road.
    float gap_x0, gap_x1;
} Barricade;

static const Barricade BARRICADES[] = {
    {CROSS_NORTH_Z, -STREET_HALF_LEN - TERRACE_WALL_THICK, true, -1.0f},
    // The lake track's way (spec 13.41).
    {CROSS_SOUTH_Z, -STREET_HALF_LEN + NEAR_END_FENCE_INSET, false, 1.0f, CROSS_X - TRACK_HALF,
     CROSS_X + TRACK_HALF},
};

// The street carried on west, the cross street across it, their sidewalks, and the corners.
static void plate(Kit* kit) {
    const float walk = STREET_HALF_WIDTH;
    const float cx0 = CROSS_X - CROSS_HALF, cx1 = CROSS_X + CROSS_HALF;
    const float wx0 = cx0 - WALK_WIDTH, wx1 = cx1 + WALK_WIDTH;
    const float east = -STREET_HALF_LEN;
    street_ground(kit, MAT_ASPHALT, CROSS_X0, cx0, -ROAD_HALF_WIDTH, ROAD_HALF_WIDTH, ROAD_Y);
    street_ground(kit, MAT_ASPHALT, cx1, east, -ROAD_HALF_WIDTH, ROAD_HALF_WIDTH, ROAD_Y);
    street_ground(kit, MAT_ASPHALT, cx0, cx1, CROSS_Z0, CROSS_Z1, ROAD_Y);
    for (int s = -1; s <= 1; s += 2) {
        const float z0 = s < 0 ? -walk : ROAD_HALF_WIDTH, z1 = s < 0 ? -ROAD_HALF_WIDTH : walk;
        street_ground(kit, MAT_CONCRETE, CROSS_X0, cx0, z0, z1, 0.0f);
        street_ground(kit, MAT_CONCRETE, cx1, east, z0, z1, 0.0f);
        // The cross street's sidewalks up each arm, and the corners behind them.
        const float a0 = s < 0 ? CROSS_Z0 : walk, a1 = s < 0 ? -walk : CROSS_Z1;
        street_ground(kit, MAT_CONCRETE, wx0, cx0, a0, a1, 0.0f);
        street_ground(kit, MAT_CONCRETE, cx1, wx1, a0, a1, 0.0f);
        street_ground(kit, MAT_DIRT, CROSS_X0, wx0, a0, a1, 0.0f);
        // Short of the terrace's west wall on the north, which stands in it.
        street_ground(kit, MAT_DIRT, wx1, s < 0 ? east - TERRACE_WALL_THICK : east, a0, a1, 0.0f);
    }
}

// One sawhorse at (x, z), turned `yaw`: two A-frames of legs, two rails striped both sides, and
// on the middle one the sign.
static void sawhorse(Kit* kit, float x, float z, float yaw, bool sign) {
    const KitFrame f = {{x, ROAD_Y, z}, yaw};
    const float half = 1.0f, spread = 0.35f, top = 1.05f;
    const float lean = atanf(spread / top), leg = 0.5f * sqrtf(top * top + spread * spread);
    for (int e = -1; e <= 1; e += 2)
        for (int s = -1; s <= 1; s += 2) {
            vec3 foot = {0.0f, 0.0f, 0.0f};
            kit_frame_point(&f, (float)e * (half - 0.1f), 0.0f, (float)s * spread, foot);
            kit_leaning_box(kit, MAT_FENCE_BOARD, foot, (vec3){0.03f, leg, 0.02f}, yaw,
                            -(float)s * lean);
        }
    const float rails[2][2] = {{0.55f, 0.75f}, {0.85f, 1.05f}};
    const float* uv = CARDS[CARD_BARRICADE_STRIPES].uv;
    for (int r = 0; r < 2; r++) {
        kit_frame_box(kit, &f, MAT_FENCE_BOARD, -half, half, rails[r][0], rails[r][1], -0.03f,
                      0.03f, false);
        for (int s = -1; s <= 1; s += 2)
            kit_frame_card_row(kit, &f, MAT_CARDS, uv, -half, half, rails[r][0], rails[r][1],
                               (float)s * 0.032f, (float)s, CARDS[CARD_BARRICADE_STRIPES].size[0]);
    }
    if (!sign)
        return;
    const float* size = CARDS[CARD_ROAD_CLOSED].size;
    const float y0 = top + 0.02f, y1 = y0 + size[1];
    kit_frame_box(kit, &f, MAT_FENCE_BOARD, -0.5f * size[0], 0.5f * size[0], y0, y1, -0.015f,
                  0.015f, false);
    for (int s = -1; s <= 1; s += 2)
        kit_frame_card_rect(kit, &f, MAT_CARDS, CARDS[CARD_ROAD_CLOSED].uv, -0.5f * size[0],
                            0.5f * size[0], y0, y1, (float)s * 0.017f, (float)s);
}

// A temporary chain-link panel from a0 to a1 along a barricade's frame: a pipe frame round the
// mesh, standing in a concrete foot at each end.
static void panel(Kit* kit, const KitFrame* f, float a0, float a1) {
    const float y0 = 0.14f, y1 = 1.85f;
    const vec3 frame[5] = {{a0 + 0.04f, y0, 0.0f},
                           {a1 - 0.04f, y0, 0.0f},
                           {a1 - 0.04f, y1, 0.0f},
                           {a0 + 0.04f, y1, 0.0f},
                           {a0 + 0.04f, y0, 0.0f}};
    kit_frame_pipe(kit, f, MAT_GALVANISED, frame, 5, 0.02f, 6);
    const vec3 mesh[4] = {{a0 + 0.04f, y0, 0.0f},
                          {a1 - 0.04f, y0, 0.0f},
                          {a1 - 0.04f, y1, 0.0f},
                          {a0 + 0.04f, y1, 0.0f}};
    kit_frame_quad(kit, f, MAT_CHAINLINK, mesh, (vec3){0.0f, 0.0f, 1.0f});
    for (int e = 0; e < 2; e++) {
        const float a = e ? a1 : a0;
        kit_frame_box(kit, f, MAT_CONCRETE, a - 0.1f, a + 0.1f, 0.0f, 0.16f, -0.3f, 0.3f, false);
    }
}

// Panels from x0 to x1 along a barricade's line at z, as many as fit 2.4 m each, a little out of
// line with one another the way they were dragged there.
static void panels(Kit* kit, float x0, float x1, float z, unsigned int* state) {
    const int n = (int)ceilf((x1 - x0) / 2.4f);
    const float step = (x1 - x0) / (float)n;
    for (int i = 0; i < n; i++) {
        const float a0 = x0 + step * (float)i, mid = a0 + 0.5f * step;
        const KitFrame f = {{mid, 0.0f, z + 0.2f * (kit_xrnd(state) - 0.5f)},
                            0.08f * (kit_xrnd(state) - 0.5f)};
        panel(kit, &f, -0.5f * step, 0.5f * step);
    }
}

static void barricade(Kit* kit, const Barricade* b, unsigned int* state) {
    const float lip = land_lip_x(b->z);
    const float cx0 = CROSS_X - CROSS_HALF, cx1 = CROSS_X + CROSS_HALF;
    panels(kit, lip + 0.6f, cx0 - 0.1f, b->z, state);
    panels(kit, cx1 + 0.1f, b->east_x - 0.05f, b->z, state);
    const bool gap = b->gap_x1 > b->gap_x0;
    // Across the road, the sign on the middle one; or either side of the gap, the sign's dragged
    // off past the west one, turned along the road.
    static const float ACROSS[3] = {CROSS_X - 2.2f, CROSS_X, CROSS_X + 2.2f};
    const float aside[3] = {b->gap_x0 - 1.2f, b->gap_x0 - 1.8f, b->gap_x1 + 1.2f};
    const float* xs = gap ? aside : ACROSS;
    const float dragged = gap ? 2.5f * b->behind : 0.0f,
                turned = gap ? 0.5f * GLM_PIf - 0.3f : 0.0f;
    for (int i = 0; i < 3; i++) {
        const float dz = 0.3f * (kit_xrnd(state) - 0.5f), yaw = 0.12f * (kit_xrnd(state) - 0.5f);
        sawhorse(kit, xs[i], b->z + dz + (i == 1 ? dragged : 0.0f), yaw + (i == 1 ? turned : 0.0f),
                 i == 1);
    }
    if (b->police)
        street_car(kit, CROSS_X + 0.4f, b->z + b->behind * 3.6f, true);
    // The body that closes the arm, half a metre past both its ends; or one either side of the
    // gap, each past its outer end.
    if (!gap) {
        kit_collider(kit, (vec3){0.5f * (lip + b->east_x), 1.5f, b->z},
                     (vec3){0.5f * (b->east_x - lip) + 0.5f, 1.5f, 0.2f}, 0.0f);
        return;
    }
    const float spans[2][2] = {{lip - 0.5f, b->gap_x0}, {b->gap_x1, b->east_x + 0.5f}};
    for (int s = 0; s < 2; s++)
        kit_collider(kit, (vec3){0.5f * (spans[s][0] + spans[s][1]), 1.5f, b->z},
                     (vec3){0.5f * (spans[s][1] - spans[s][0]), 1.5f, 0.2f}, 0.0f);
}

// What is left of the road where it broke off: slabs gone over the edge, the kerbs snapped,
// rebar out of the broken edge, and a drain pipe cut through below.
static void broken_end(Kit* kit, unsigned int* state) {
    const float edge = CROSS_X0 + 0.05f;
    const float road = STREET_HALF_WIDTH;
    for (float z = -road + 0.6f; z < road - 0.5f; z += 1.3f + 0.5f * kit_xrnd(state)) {
        const bool walk = fabsf(z) > ROAD_HALF_WIDTH;
        const float thick = walk ? 0.12f : 0.18f, top = walk ? 0.0f : ROAD_Y;
        const float len = 0.8f + 1.2f * kit_xrnd(state),
                    droop = glm_rad(20.0f + 45.0f * kit_xrnd(state));
        const vec3 base = {edge, top - thick, z};
        kit_leaning_box(kit, walk ? MAT_CONCRETE : MAT_ASPHALT, base,
                        (vec3){0.45f + 0.25f * kit_xrnd(state), 0.5f * len, 0.5f * thick},
                        -0.5f * GLM_PIf + 0.3f * (kit_xrnd(state) - 0.5f), 0.5f * GLM_PIf + droop);
    }
    // The kerbs, each snapped off and hanging by its reinforcement.
    for (int s = -1; s <= 1; s += 2)
        kit_leaning_box(kit, MAT_CONCRETE,
                        (vec3){edge, -0.3f, (float)s * (ROAD_HALF_WIDTH + 0.08f)},
                        (vec3){0.08f, 0.45f, 0.15f}, -0.5f * GLM_PIf, 0.5f * GLM_PIf + 0.9f);
    for (int i = 0; i < 12; i++) {
        const float z = -road + 0.4f + (2.0f * road - 0.8f) * kit_xrnd(state);
        const float y = (fabsf(z) > ROAD_HALF_WIDTH ? 0.0f : ROAD_Y) - 0.1f;
        const float reach = 0.4f + 1.0f * kit_xrnd(state);
        const vec3 bar[3] = {{edge, y, z},
                             {edge - 0.5f * reach, y - 0.05f, z + 0.1f * (kit_xrnd(state) - 0.5f)},
                             {edge - reach, y - 0.2f - 0.6f * kit_xrnd(state), z}};
        kit_frame_pipe(kit, &KIT_WORLD, MAT_LAMP_POST, bar, 3, 0.008f, 4);
    }
    // A storm drain cut through where the road went, its mouth dark, still running in the rain.
    const vec3 mouth = {CROSS_X0 - 1.2f, -2.2f, 1.6f};
    const vec3 pipe[2] = {{CROSS_X0 + 0.5f, mouth[1], mouth[2]}, {mouth[0], mouth[1], mouth[2]}};
    kit_frame_pipe(kit, &KIT_WORLD, MAT_CONCRETE, pipe, 2, 0.36f, 12);
    vec3 disc[12];
    for (int i = 0; i < 12; i++) {
        const float a = 2.0f * GLM_PIf * (float)i / 12.0f;
        disc[i][0] = mouth[0] - 0.01f;
        disc[i][1] = mouth[1] + 0.29f * cosf(a);
        disc[i][2] = mouth[2] + 0.29f * sinf(a);
    }
    kit_polygon_facing(kit, MAT_BLACK, disc, 12, (vec3){-1.0f, 0.0f, 0.0f});
    const vec3 lip = {mouth[0] - 0.05f, mouth[1] - 0.3f, mouth[2]};
    kit_drip(kit, &KIT_WORLD, lip, lip, 2.0f, -60.0f);
}

// A W-beam guard rail across the road's end on posts, one length of it torn from its post and
// bent out over the edge, and a body the rail's length so nobody steps past it.
static void guard_rail(Kit* kit) {
    const float road = STREET_HALF_WIDTH - 0.3f, tear = 1.4f;
    const vec2 beam[6] = {{0.0f, 0.48f}, {0.07f, 0.53f},  {0.07f, 0.66f},
                          {0.0f, 0.71f}, {-0.01f, 0.71f}, {-0.01f, 0.48f}};
    // Its frame runs along +z with d out over the edge, toward -x.
    const KitFrame f = {{RAIL_X, 0.0f, 0.0f}, -0.5f * GLM_PIf};
    kit_frame_run(kit, &f, MAT_GALVANISED, beam, 6, -road, tear);
    for (float a = -road + 0.2f; a < road; a += 1.9f)
        kit_frame_box(kit, &f, MAT_GALVANISED, a - 0.04f, a + 0.04f, 0.0f, 0.75f, -0.18f, -0.02f,
                      false);
    // The torn length, swung out about the end still bolted on.
    KitFrame bent = f;
    kit_frame_point(&f, tear, 0.0f, 0.0f, bent.origin);
    bent.yaw = f.yaw + 0.6f;
    kit_frame_run(kit, &bent, MAT_GALVANISED, beam, 6, 0.0f, road - tear + 0.6f);
    kit_collider(kit, (vec3){RAIL_X, 0.75f, 0.0f}, (vec3){0.2f, 0.75f, road + 0.3f}, 0.0f);
}

/*
 * The last pole, at the lip, leaning out over the chasm, its wires from the street's westmost pole
 * sagging to it and on from it down into the fog.
 */
static void leaning_pole(Kit* kit) {
    const vec3 base = {CROSS_X0 + 0.6f, 0.0f, POLE_Z};
    vec3 dir = {-sinf(glm_rad(24.0f)), cosf(glm_rad(24.0f)), -0.12f};
    glm_vec3_normalize(dir);
    vec3 top = {0.0f, 0.0f, 0.0f};
    glm_vec3_scale(dir, 8.5f, top);
    glm_vec3_add(top, (float*)base, top);
    const vec3 pole[2] = {{base[0], base[1] - 0.3f, base[2]}, {top[0], top[1], top[2]}};
    kit_frame_pipe(kit, &KIT_WORLD, MAT_POLE, pole, 2, 0.12f, 8);
    const vec3 arm[2] = {{top[0], top[1] - 0.5f, top[2] - 0.8f},
                         {top[0], top[1] - 0.5f, top[2] + 0.8f}};
    kit_frame_pipe(kit, &KIT_WORLD, MAT_POLE, arm, 2, 0.05f, 6);
    kit_collider(kit, (vec3){base[0], 1.0f, base[2]}, (vec3){0.15f, 1.0f, 0.15f}, 0.0f);
    for (int w = -1; w <= 1; w += 2) {
        const float off = POLE_WIRE_OFF * (float)w;
        const vec3 from = {POLE_WEST_X, POLE_WIRE_Y, POLE_Z + off};
        const vec3 to = {top[0], top[1] - 0.45f, top[2] + off};
        // A catenary's sag, as a parabola through eight points.
        vec3 span[8];
        for (int i = 0; i < 8; i++) {
            const float t = (float)i / 7.0f;
            glm_vec3_lerp((float*)from, (float*)to, t, span[i]);
            span[i][1] -= 1.1f * 4.0f * t * (1.0f - t);
        }
        kit_frame_pipe(kit, &KIT_WORLD, MAT_BLACK, span, 8, 0.012f, 4);
        // Snapped on the far side, hanging down into the hole.
        const vec3 hang[5] = {{to[0], to[1], to[2]},
                              {to[0] - 1.2f, to[1] - 1.5f, to[2] + 0.3f * (float)w},
                              {to[0] - 2.2f, to[1] - 4.5f, to[2] + 0.5f * (float)w},
                              {to[0] - 2.6f, to[1] - 8.5f, to[2] + 0.6f * (float)w},
                              {to[0] - 2.7f, to[1] - 13.0f, to[2] + 0.6f * (float)w}};
        kit_frame_pipe(kit, &KIT_WORLD, MAT_BLACK, hang, 5, 0.012f, 4);
    }
}

void crossroads_build(Kit* kit, Scene* scene, bool night, FailingLamp* failing) {
    unsigned int state = 0x5eed1335u;
    plate(kit);
    for (int i = 0; i < KIT_COUNT(BARRICADES); i++)
        barricade(kit, &BARRICADES[i], &state);
    broken_end(kit, &state);
    guard_rail(kit);
    leaning_pole(kit);
    // A lamp on our side near the edge, its arm out over the road, failing.
    Light* light = street_lamp(kit, scene, CROSS_X0 + 4.5f, 0.0f, ROAD_HALF_WIDTH + 1.4f, GLM_PIf,
                               night, false, street_lamp_profile(scene, night));
    *failing = failing_lamp(light, 1u);
}
