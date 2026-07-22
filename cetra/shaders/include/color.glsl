// YCoCg transform pair, shared by the two temporal accumulators that
// neighborhood-clamp in this space (taa_resolve, ssgi_accum). Clamping in
// YCoCg rather than RGB keeps the clamp from shifting hue, which is why both
// passes convert.
//
// These two functions are exact inverses of each other, so they have to move
// together -- which is the argument for one copy.
//
// Shared here and NOT the surrounding clamp/blend code: that code deliberately
// differs between the two (Catmull-Rom history reprojection for TAA, bilinear
// for SSGI, documented at ssgi_accum_frag.glsl:13-15), and merging it would
// erase an intentional difference rather than a duplicated one.

vec3 rgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 yCoCgToRgb(vec3 c) {
    float t = c.x - c.z;
    return vec3(t + c.y, c.x + c.z, t - c.y);
}
