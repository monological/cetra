#ifndef _WATER_BOUNDS_H_
#define _WATER_BOUNDS_H_

#include <stdbool.h>

/*
 * Whether a sea bounded by `b` -- Water.bounds, world XZ, min x, min z, max x, max z -- reaches
 * (x, z); a zero rectangle bounds nothing (spec 13.41).
 *
 * A header of its own with no GL in it, so rain.c, which has none, asks the same question
 * water.c does. The shaders ask include/water_bounds.glsl.
 */
static inline bool water_bounds_cover(const float b[4], float x, float z) {
    if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 0.0f)
        return true;
    return x >= b[0] && z >= b[1] && x <= b[2] && z <= b[3];
}

#endif
