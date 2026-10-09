// Where a sea is (spec 13.41): Water.bounds, a rectangle in world XZ, min x and z then max.
//
// Its own chunk because three programs ask it of three different uniforms -- the water and every
// lit surface through water_light.glsl, the rain through its own -- and the rule that a zero
// rectangle bounds nothing is said once.

// Whether a sea bounded by `b` reaches `xz`.
bool waterBoundsCover(vec4 b, vec2 xz) {
    return b == vec4(0.0) || (all(greaterThanEqual(xz, b.xy)) && all(lessThanEqual(xz, b.zw)));
}
