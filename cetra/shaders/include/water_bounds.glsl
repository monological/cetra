// Where a sea is (spec 13.41): Water.bounds, a rectangle in world XZ, min x and z then max, where a
// zero rectangle bounds nothing, less its dry shapes (spec 13.56). water_bounds.h is the same rule
// for the C side.

#include "water_dry_constants.glsl"

// The water's dry shapes, two vec4s each: the centre's world xz and the unit xz direction its
// length runs along, then its half length, its half width and its superellipse's power. A count
// of 0, which is what a program never told of any holds, is no dry shape at all.
uniform int waterDryCount;
uniform vec4 waterDry[2 * WATER_DRY_MAX];

// Whether a sea bounded by `b` reaches `xz`.
bool waterBoundsCover(vec4 b, vec2 xz) {
    return b == vec4(0.0) || (all(greaterThanEqual(xz, b.xy)) && all(lessThanEqual(xz, b.zw)));
}

// Whether a dry shape covers `xz`.
bool waterDryCovers(vec2 xz) {
    for (int i = 0; i < waterDryCount; i++) {
        vec4 at = waterDry[2 * i], size = waterDry[2 * i + 1];
        vec2 d = xz - at.xy;
        vec2 q = abs(vec2(dot(d, at.zw), at.z * d.y - at.w * d.x) / size.xy);
        if (pow(q.x, size.z) + pow(q.y, size.z) <= 1.0)
            return true;
    }
    return false;
}

// Whether a sea bounded by `b`, less its dry shapes, reaches `xz`.
bool waterReaches(vec4 b, vec2 xz) {
    return waterBoundsCover(b, xz) && !waterDryCovers(xz);
}

// Whether `p` is under a sea standing at `level` where waterReaches says it is.
bool waterUnder(float level, vec4 b, vec3 p) {
    return p.y < level && waterReaches(b, p.xz);
}
