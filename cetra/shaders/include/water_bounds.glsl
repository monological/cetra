// Where a sea is (spec 13.41): Water.bounds, a rectangle in world XZ, min x and z then max, where a
// zero rectangle bounds nothing. water_bounds.h is the same rule for the C side.

// Whether a sea bounded by `b` reaches `xz`.
bool waterBoundsCover(vec4 b, vec2 xz) {
    return b == vec4(0.0) || (all(greaterThanEqual(xz, b.xy)) && all(lessThanEqual(xz, b.zw)));
}

// Whether `p` is under a sea standing at `level` inside `b`.
bool waterUnder(float level, vec4 b, vec3 p) {
    return p.y < level && waterBoundsCover(b, p.xz);
}
