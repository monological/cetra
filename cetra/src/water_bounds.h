#ifndef _WATER_BOUNDS_H_
#define _WATER_BOUNDS_H_

#include <stdbool.h>

/*
 * Where a sea is (spec 13.41): Water.bounds, world XZ, min x, min z, max x, max z, where a zero
 * rectangle bounds nothing. No GL, so code with none asks it too; include/water_bounds.glsl is the
 * same rule for the shaders.
 */

// Whether a sea bounded by `b` reaches (x, z).
static inline bool water_bounds_cover(const float b[4], float x, float z) {
    if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 0.0f)
        return true;
    return x >= b[0] && z >= b[1] && x <= b[2] && z <= b[3];
}

// Whether (x, y, z) is under a sea standing at `level` inside `b`.
static inline bool water_under(float level, const float b[4], float x, float y, float z) {
    return y < level && water_bounds_cover(b, x, z);
}

#endif
