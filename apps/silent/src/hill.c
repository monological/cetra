#include <float.h>
#include <math.h>

#include "hill.h"
#include "layout.h"
#include "mats.h"
#include "road.h"

/*
 * The hill past the street's east end (spec 13.25): it rises from the street's level to the
 * mansion's grounds at MANSION_Y, with the drive carved into it as it winds up.
 *
 * This is the hill's HEIGHT and the drive, not the ground drawn over them (spec 13.35). The
 * drive's surface is the height's own along the road, so the asphalt ribbon laid on it and the
 * ground under it agree. Puddles stand on ground within 8 degrees of level, and the drive climbs
 * at under that.
 */

// Where the hill starts: the street's own end, and where it is still at the street's level.
#define HILL_X0    STREET_HALF_LEN
#define HILL_FLAT  (STREET_HALF_LEN + 13.0f)
#define HILL_REACH 60.0f // metres from the grounds over which the hill falls to the street
#define HILL_NOISE 0.7f  // metres of lumps, none on the drive or the grounds

// The drive's line, west to east and up: out of the street's end, a bend north, a switchback
// back west, and east along the grounds' front to the gate, which is in line with the house's
// front path.
static const float DRIVE_POINTS[][2] = {
    {STREET_HALF_LEN, 0.0f}, {60.0f, -1.0f},
    {76.0f, 5.0f},           {86.0f, 18.0f},
    {80.0f, 30.0f},          {92.0f, 35.0f},
    {104.0f, 33.5f},         {MANSION_X + 0.5f * (PATH_X0 + PATH_X1), GROUNDS_Z0 - 0.4f},
};

// The street's road level at its start, the grounds at its end, a climb between that eases in
// and out, and 2.6 m of asphalt either side.
static const RoadDesc DRIVE = {
    .points = DRIVE_POINTS,
    .point_count = (int)(sizeof(DRIVE_POINTS) / sizeof(DRIVE_POINTS[0])),
    .half = 2.6f,
    .shoulder = 4.0f,
    .y0 = ROAD_Y,
    .y1 = MANSION_Y,
    .flat_run = 8.0f, // level at the street's end, before it starts to climb
    .flat_end = 6.0f, // and level at the gate
    .rise = ROAD_RISE_SMOOTH,
};

static Road g_drive = {.desc = &DRIVE};

// A smooth lump field: value noise over a lattice, two octaves. Hashed from the lattice point,
// so the hill is the same on every run.
static float lattice(int ix, int iz) {
    unsigned int h = (unsigned int)ix * 73856093u ^ (unsigned int)iz * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffffu) / 65535.0f * 2.0f - 1.0f;
}

static float value_noise(float x, float z) {
    const int ix = (int)floorf(x), iz = (int)floorf(z);
    const float fx = glm_smoothstep(0.0f, 1.0f, x - (float)ix);
    const float fz = glm_smoothstep(0.0f, 1.0f, z - (float)iz);
    const float a = lattice(ix, iz), b = lattice(ix + 1, iz);
    const float c = lattice(ix, iz + 1), e = lattice(ix + 1, iz + 1);
    return (a + (b - a) * fx) + ((c + (e - c) * fx) - (a + (b - a) * fx)) * fz;
}

static float grounds_distance(float x, float z) {
    return plan_box_distance(x, z, GROUNDS_X0, GROUNDS_X1, GROUNDS_Z0, GROUNDS_Z1);
}

bool hill_on_grounds(float x, float z) {
    return grounds_distance(x, z) <= 0.0f;
}

float hill_lumps(float x, float z) {
    return value_noise(x * 0.08f, z * 0.08f) + 0.5f * value_noise(x * 0.2f, z * 0.2f);
}

float hill_height(float x, float z) {
    const float g = grounds_distance(x, z);
    if (g <= 0.0f)
        return MANSION_Y;
    const float rise = glm_smoothstep(HILL_X0, HILL_FLAT, x);
    float h = MANSION_Y * (1.0f - glm_smoothstep(0.0f, HILL_REACH, g)) * rise;
    h += HILL_NOISE * hill_lumps(x, z) * rise * glm_smoothstep(0.0f, 6.0f, g);
    return road_carve(&g_drive, x, z, h);
}

float hill_drive_distance(float x, float z) {
    float along = 0.0f;
    return road_distance(&g_drive, x, z, &along);
}

void hill_drive_point(float t, float* x, float* z) {
    road_point(&g_drive, t, x, z);
}

void hill_drive_frame(float t, float* x, float* z, float* dir_x, float* dir_z) {
    road_frame(&g_drive, t, x, z, dir_x, dir_z);
}

void hill_build(Kit* kit) {
    // The grounds stand on a box: their flat would be one long run of coplanar triangles in the
    // land's collider, which leaves them out.
    const vec3 centre = {0.5f * (GROUNDS_X0 + GROUNDS_X1), MANSION_Y - 0.5f,
                         0.5f * (GROUNDS_Z0 + GROUNDS_Z1)};
    const vec3 half = {0.5f * (GROUNDS_X1 - GROUNDS_X0), 0.5f, 0.5f * (GROUNDS_Z1 - GROUNDS_Z0)};
    kit_collider(kit, centre, half, 0.0f);

    road_ribbon(kit, &g_drive, MAT_ASPHALT, 0.0f, FLT_MAX);
}
