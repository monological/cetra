#include <math.h>

#include "hill.h"
#include "layout.h"
#include "mats.h"

/*
 * The hill past the street's east end (spec 13.25): it rises from the street's level to the
 * mansion's grounds at MANSION_Y, with the drive carved into it as it winds up.
 *
 * This is the hill's HEIGHT and the drive; the ground drawn and collided over it is land.c's grid
 * (spec 13.35), which reads hill_height as one of its terms. The drive's surface is that
 * function's own along the road, so the asphalt ribbon laid on it and the ground under it agree.
 * Puddles stand on ground within 8 degrees of level, and the drive climbs at under that.
 */

// Where the hill starts: the street's own end, and where it is still at the street's level.
#define HILL_X0    STREET_HALF_LEN
#define HILL_FLAT  (STREET_HALF_LEN + 13.0f)
#define HILL_REACH 60.0f // metres from the grounds over which the hill falls to the street
#define HILL_NOISE 0.7f  // metres of lumps, none on the drive or the grounds

#define DRIVE_HALF     2.6f // the asphalt's half-width
#define DRIVE_SHOULDER 4.0f // over which the ground comes back up to the hill's own
#define DRIVE_SAMPLES  512
#define DRIVE_STEP     0.5f // metres between the drive's samples
#define DRIVE_FLAT_RUN 8.0f // level at the street's end, before it starts to climb
#define DRIVE_FLAT_END 6.0f // and level at the gate

// The drive's line, west to east and up: out of the street's end, a bend north, a switchback
// back west, and east along the grounds' front to the gate, which is in line with the house's
// front path.
static const float DRIVE_POINTS[][2] = {
    {STREET_HALF_LEN, 0.0f}, {60.0f, -1.0f},
    {76.0f, 5.0f},           {86.0f, 18.0f},
    {80.0f, 30.0f},          {92.0f, 35.0f},
    {104.0f, 33.5f},         {MANSION_X + 0.5f * (PATH_X0 + PATH_X1), GROUNDS_Z0 - 0.4f},
};
#define DRIVE_POINT_COUNT ((int)(sizeof(DRIVE_POINTS) / sizeof(DRIVE_POINTS[0])))

typedef struct Drive {
    float x[DRIVE_SAMPLES], z[DRIVE_SAMPLES], s[DRIVE_SAMPLES];
    int count;
    float length;
} Drive;

static Drive g_drive;

static float catmull(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// The drive's line at even steps of its length, through the points by Catmull-Rom.
static void drive_sample(Drive* d) {
    // Densely first, then resampled by length, so the steps are even whatever the spans are.
    enum { DENSE = 64 };
    float px[(DRIVE_POINT_COUNT - 1) * DENSE + 1], pz[(DRIVE_POINT_COUNT - 1) * DENSE + 1],
        ps[(DRIVE_POINT_COUNT - 1) * DENSE + 1];
    int n = 0;
    for (int i = 0; i + 1 < DRIVE_POINT_COUNT; i++) {
        const int i0 = i > 0 ? i - 1 : 0, i3 = i + 2 < DRIVE_POINT_COUNT ? i + 2 : i + 1;
        for (int k = 0; k < DENSE; k++) {
            const float t = (float)k / DENSE;
            px[n] = catmull(DRIVE_POINTS[i0][0], DRIVE_POINTS[i][0], DRIVE_POINTS[i + 1][0],
                            DRIVE_POINTS[i3][0], t);
            pz[n] = catmull(DRIVE_POINTS[i0][1], DRIVE_POINTS[i][1], DRIVE_POINTS[i + 1][1],
                            DRIVE_POINTS[i3][1], t);
            n++;
        }
    }
    px[n] = DRIVE_POINTS[DRIVE_POINT_COUNT - 1][0];
    pz[n] = DRIVE_POINTS[DRIVE_POINT_COUNT - 1][1];
    n++;
    ps[0] = 0.0f;
    for (int i = 1; i < n; i++)
        ps[i] = ps[i - 1] + hypotf(px[i] - px[i - 1], pz[i] - pz[i - 1]);

    d->length = ps[n - 1];
    d->count = 0;
    int j = 0;
    for (float s = 0.0f; d->count < DRIVE_SAMPLES; s += DRIVE_STEP) {
        if (s > d->length)
            s = d->length;
        while (j + 1 < n - 1 && ps[j + 1] < s)
            j++;
        const float span = ps[j + 1] - ps[j];
        const float t = span > 0.0f ? (s - ps[j]) / span : 0.0f;
        d->x[d->count] = px[j] + (px[j + 1] - px[j]) * t;
        d->z[d->count] = pz[j] + (pz[j + 1] - pz[j]) * t;
        d->s[d->count] = s;
        d->count++;
        if (s >= d->length)
            break;
    }
}

// The drive's height along it: the street's road at its start, the grounds at its end, and a
// climb between that eases in and out.
static float drive_height(float s) {
    const float t = glm_smoothstep(DRIVE_FLAT_RUN, g_drive.length - DRIVE_FLAT_END, s);
    return ROAD_Y + (MANSION_Y - ROAD_Y) * t;
}

// How far (x, z) is from the drive's line, and how far along it the nearest point is.
static float drive_distance(float x, float z, float* along) {
    float best = 1e30f;
    *along = 0.0f;
    for (int i = 0; i + 1 < g_drive.count; i++) {
        const float ax = g_drive.x[i], az = g_drive.z[i];
        const float bx = g_drive.x[i + 1] - ax, bz = g_drive.z[i + 1] - az;
        const float len2 = bx * bx + bz * bz;
        float t = len2 > 0.0f ? ((x - ax) * bx + (z - az) * bz) / len2 : 0.0f;
        t = glm_clamp(t, 0.0f, 1.0f);
        const float dx = x - (ax + bx * t), dz = z - (az + bz * t);
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
            *along = g_drive.s[i] + (g_drive.s[i + 1] - g_drive.s[i]) * t;
        }
    }
    return sqrtf(best);
}

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
    const float dx = fmaxf(fmaxf(GROUNDS_X0 - x, x - GROUNDS_X1), 0.0f);
    const float dz = fmaxf(fmaxf(GROUNDS_Z0 - z, z - GROUNDS_Z1), 0.0f);
    return sqrtf(dx * dx + dz * dz);
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

    float along = 0.0f;
    const float d = drive_distance(x, z, &along);
    const float w = 1.0f - glm_smoothstep(DRIVE_HALF, DRIVE_HALF + DRIVE_SHOULDER, d);
    // A hair under the asphalt, so the ribbon laid on it is never under the ground.
    return h + (drive_height(along) - 0.03f - h) * w;
}

float hill_drive_distance(float x, float z) {
    float along = 0.0f;
    return drive_distance(x, z, &along);
}

void hill_drive_point(float t, float* x, float* z) {
    const int i = (int)(glm_clamp(t, 0.0f, 1.0f) * (float)(g_drive.count - 1));
    *x = g_drive.x[i];
    *z = g_drive.z[i];
}

void hill_drive_frame(float t, float* x, float* z, float* dir_x, float* dir_z) {
    const int i = (int)(glm_clamp(t, 0.0f, 1.0f) * (float)(g_drive.count - 1));
    const int a = i > 0 ? i - 1 : 0, b = i + 1 < g_drive.count ? i + 1 : i;
    *x = g_drive.x[i];
    *z = g_drive.z[i];
    const float dx = g_drive.x[b] - g_drive.x[a], dz = g_drive.z[b] - g_drive.z[a];
    const float len = hypotf(dx, dz);
    *dir_x = len > 0.0f ? dx / len : 1.0f;
    *dir_z = len > 0.0f ? dz / len : 0.0f;
}

// The asphalt: a ribbon along the drive's samples, level across.
static void drive_ribbon(Kit* kit) {
    const vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 prev_l = {0}, prev_r = {0};
    for (int i = 0; i < g_drive.count; i++) {
        const int a = i > 0 ? i - 1 : 0, b = i + 1 < g_drive.count ? i + 1 : i;
        float tx = g_drive.x[b] - g_drive.x[a], tz = g_drive.z[b] - g_drive.z[a];
        const float len = hypotf(tx, tz);
        tx /= len;
        tz /= len;
        const float y = drive_height(g_drive.s[i]);
        const vec3 l = {g_drive.x[i] - tz * DRIVE_HALF, y, g_drive.z[i] + tx * DRIVE_HALF};
        const vec3 r = {g_drive.x[i] + tz * DRIVE_HALF, y, g_drive.z[i] - tx * DRIVE_HALF};
        if (i > 0)
            kit_quad_facing(kit, MAT_ASPHALT, prev_l, prev_r, r, l, up);
        glm_vec3_copy((float*)l, prev_l);
        glm_vec3_copy((float*)r, prev_r);
    }
}

void hill_build(Kit* kit) {
    drive_sample(&g_drive);

    // The grounds stand on a box: their flat would be one long run of coplanar triangles in the
    // land's collider, which leaves them out.
    const vec3 centre = {0.5f * (GROUNDS_X0 + GROUNDS_X1), MANSION_Y - 0.5f,
                         0.5f * (GROUNDS_Z0 + GROUNDS_Z1)};
    const vec3 half = {0.5f * (GROUNDS_X1 - GROUNDS_X0), 0.5f, 0.5f * (GROUNDS_Z1 - GROUNDS_Z0)};
    kit_collider(kit, centre, half, 0.0f);

    drive_ribbon(kit);
}
