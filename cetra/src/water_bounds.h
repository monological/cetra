#ifndef _WATER_BOUNDS_H_
#define _WATER_BOUNDS_H_

#include <math.h>
#include <stdbool.h>

#include "../shaders/include/water_dry_constants.glsl"

/*
 * Where a sea is (spec 13.41): Water.bounds, world XZ, min x, min z, max x, max z, where a zero
 * rectangle bounds nothing, less its dry shapes (spec 13.56). No GL, so code with none asks it
 * too; include/water_bounds.glsl is the same rule for the shaders.
 */

// A place inside a sea's bounds where it is not -- the inside of a hull floating on it, which
// sits below the level -- a superellipse in world XZ: |a / half[0]|^power + |c / half[1]|^power
// <= 1, a along `along` from the centre and c across it.
typedef struct WaterDry {
    float centre[2]; // world x, z
    float along[2];  // the unit xz direction its length runs along
    float half[2];   // half its length along `along`, and half its width across it
    float power;     // 2 is an ellipse; higher is squarer
} WaterDry;

// Whether a sea bounded by `b` reaches (x, z).
static inline bool water_bounds_cover(const float b[4], float x, float z) {
    if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 0.0f)
        return true;
    return x >= b[0] && z >= b[1] && x <= b[2] && z <= b[3];
}

// Whether any of `count` dry shapes covers (x, z).
static inline bool water_dry_covers(const WaterDry* dry, int count, float x, float z) {
    for (int i = 0; i < count; i++) {
        const WaterDry* d = &dry[i];
        if (d->half[0] <= 0.0f || d->half[1] <= 0.0f)
            continue;
        const float dx = x - d->centre[0], dz = z - d->centre[1];
        const float a = (dx * d->along[0] + dz * d->along[1]) / d->half[0];
        const float c = (dz * d->along[0] - dx * d->along[1]) / d->half[1];
        if (powf(fabsf(a), d->power) + powf(fabsf(c), d->power) <= 1.0f)
            return true;
    }
    return false;
}

// `count` dry shapes as the shaders' `waterDry` array takes them, two vec4s each, leaving out any
// of no size; how many it packed.
static inline int water_dry_pack(const WaterDry* dry, int count, float out[2 * WATER_DRY_MAX][4]) {
    int n = 0;
    for (int i = 0; i < count && i < WATER_DRY_MAX; i++) {
        const WaterDry* d = &dry[i];
        if (d->half[0] <= 0.0f || d->half[1] <= 0.0f)
            continue;
        float* a = out[2 * n];
        float* b = out[2 * n + 1];
        a[0] = d->centre[0], a[1] = d->centre[1], a[2] = d->along[0], a[3] = d->along[1];
        b[0] = d->half[0], b[1] = d->half[1], b[2] = d->power, b[3] = 0.0f;
        n++;
    }
    return n;
}

// Whether a sea bounded by `b`, less `count` dry shapes, reaches (x, z).
static inline bool water_reaches(const float b[4], const WaterDry* dry, int count, float x,
                                 float z) {
    return water_bounds_cover(b, x, z) && !water_dry_covers(dry, count, x, z);
}

// Whether (x, y, z) is under a sea standing at `level` where water_reaches says it is.
static inline bool water_under(float level, const float b[4], const WaterDry* dry, int count,
                               float x, float y, float z) {
    return y < level && water_reaches(b, dry, count, x, z);
}

#endif
