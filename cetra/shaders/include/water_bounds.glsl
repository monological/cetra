// Where a sea is (spec 13.41): a rectangle in world XZ, min x and z then max, where a zero
// rectangle bounds nothing, less its dry shapes (spec 13.56). water_bounds.h is the same rule
// for the C side, and water_domain_upload sets these.

#include "water_dry_constants.glsl"

uniform vec4 waterBounds;
// The water's dry shapes, two vec4s each: the centre's world xz and the unit xz direction its
// length runs along, then its half length, its half width and its superellipse's power. A count
// of 0, which is what a program never told of any holds, is no dry shape at all.
uniform int waterDryCount;
uniform vec4 waterDry[2 * WATER_DRY_MAX];

// Whether the bounds cover `xz`.
bool waterBoundsCover(vec2 xz) {
    return waterBounds == vec4(0.0) ||
           (all(greaterThanEqual(xz, waterBounds.xy)) && all(lessThanEqual(xz, waterBounds.zw)));
}

// Whether a dry shape covers `xz`. A superellipse lies inside its box, so the box turns most
// points away before the powers.
bool waterDryCovers(vec2 xz) {
    for (int i = 0; i < waterDryCount; i++) {
        vec4 at = waterDry[2 * i], size = waterDry[2 * i + 1];
        vec2 d = xz - at.xy;
        vec2 q = abs(vec2(dot(d, at.zw), at.z * d.y - at.w * d.x) / size.xy);
        if (max(q.x, q.y) <= 1.0 && pow(q.x, size.z) + pow(q.y, size.z) <= 1.0)
            return true;
    }
    return false;
}

// Whether the sea reaches `xz`.
bool waterReaches(vec2 xz) {
    return waterBoundsCover(xz) && !waterDryCovers(xz);
}

// Whether `p` is under a sea standing at `level` where waterReaches says it is.
bool waterUnder(float level, vec3 p) {
    return p.y < level && waterReaches(p.xz);
}
