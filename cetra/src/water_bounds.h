#ifndef _WATER_BOUNDS_H_
#define _WATER_BOUNDS_H_

#include <math.h>
#include <stdbool.h>
#include <cglm/cglm.h>

#include "../shaders/include/water_dry_constants.glsl"

/*
 * Where a sea is (spec 13.41): a rectangle in world XZ, where a zero rectangle bounds nothing,
 * less its dry shapes (spec 13.56). No GL, so code with none asks it too;
 * include/water_bounds.glsl is the same rule for the shaders.
 */

// A place inside a sea's bounds where it is not -- the inside of a hull floating on it, which
// sits below the level -- a superellipse in world XZ: |a / half[0]|^power + |c / half[1]|^power
// <= 1, a along `along` from the centre and c across it. Laid out as the shaders' two vec4s.
typedef struct WaterDry {
    float centre[2]; // world x, z
    float along[2];  // the unit xz direction its length runs along
    float half[2];   // half its length along `along`, and half its width across it; both > 0
    float power;     // 2 is an ellipse; higher is squarer; over 0
    float unused;
} WaterDry;

_Static_assert(sizeof(WaterDry) == 8 * sizeof(float), "WaterDry is two vec4s");

typedef struct WaterDomain {
    vec4 bounds;                 // world XZ: min x, min z, max x, max z; all zero = everywhere
    WaterDry dry[WATER_DRY_MAX]; // where inside the bounds it is not; the first dry_count are read
    int dry_count;
} WaterDomain;

// A dry shape centred on (x, z), its length turned `yaw` radians about +Y from +Z.
static inline WaterDry water_dry_make(float x, float z, float yaw, float half_length,
                                      float half_width, float power) {
    return (WaterDry){.centre = {x, z},
                      .along = {sinf(yaw), cosf(yaw)},
                      .half = {half_length, half_width},
                      .power = power};
}

// Whether the bounds `b` are the zero rectangle, which bounds nothing.
static inline bool water_bounds_everywhere(const vec4 b) {
    return b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 0.0f;
}

// Whether the bounds `b` cover (x, z).
static inline bool water_bounds_cover(const vec4 b, float x, float z) {
    if (water_bounds_everywhere(b))
        return true;
    return x >= b[0] && z >= b[1] && x <= b[2] && z <= b[3];
}

// Whether one of the domain's dry shapes covers (x, z).
static inline bool water_dry_covers(const WaterDomain* d, float x, float z) {
    for (int i = 0; i < d->dry_count; i++) {
        const WaterDry* s = &d->dry[i];
        const float dx = x - s->centre[0], dz = z - s->centre[1];
        const float a = fabsf(dx * s->along[0] + dz * s->along[1]) / s->half[0];
        const float c = fabsf(dz * s->along[0] - dx * s->along[1]) / s->half[1];
        if (a <= 1.0f && c <= 1.0f && powf(a, s->power) + powf(c, s->power) <= 1.0f)
            return true;
    }
    return false;
}

// Whether the sea reaches (x, z); a NULL domain is a sea everywhere.
static inline bool water_reaches(const WaterDomain* d, float x, float z) {
    return !d || (water_bounds_cover(d->bounds, x, z) && !water_dry_covers(d, x, z));
}

// Whether (x, y, z) is under a sea standing at `level` where water_reaches says it is.
static inline bool water_under(float level, const WaterDomain* d, float x, float y, float z) {
    return y < level && water_reaches(d, x, z);
}

struct UniformManager;
// The domain to a program that includes water_bounds.glsl; a NULL domain is a sea everywhere.
void water_domain_upload(const WaterDomain* domain, struct UniformManager* u);

#endif
