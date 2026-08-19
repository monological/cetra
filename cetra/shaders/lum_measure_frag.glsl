#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Auto-exposure step 1: down-measure the linear HDR scene into a small target
// storing log2 luminance, for lum_histogram_frag to bin and lum_reduce_frag to
// collapse.
//
// This target used to be mipped down instead, its top mip holding the scene's
// geometric mean. A mean cannot have a tail cut off it after the fact, which is
// what a percentile needs, so 11.52 replaced the mip chain with the two passes
// above -- and the sum each bin carries makes keeping the whole population
// reproduce that mean exactly.
#include "view.glsl"

uniform sampler2D hdrTex; // Resolved linear HDR scene

void main()
{
    // Back to absolute scene radiance first. The buffer is pre-exposed
    // (view.glsl), and metering it in working space would measure this pass's
    // own contribution to the exposure that produced it -- a feedback loop that
    // ratchets toward the clamp instead of settling. Everything downstream --
    // the bin range, the percentiles, the key -- is stated in absolute cd/m^2
    // and only means anything against absolute input.
    // Sanitize BEFORE converting: a +INF texel would poison the bin sum it lands
    // in, and the ceiling is the one pbr_frag already wrote
    // under, so it clips nothing a shading pass let through. Applying it after
    // the conversion would instead cap absolute radiance at 60000, which a noon
    // sun legitimately exceeds -- the meter would read it as overcast.
    vec3 hdr = min(texture(hdrTex, TexCoords).rgb, vec3(WS_SCENE_MAX)) * oneOverPreExposure;
    float lum = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    // A numeric guard: log2(0) is -INF, which would poison the histogram's sum
    // and reach the CPU as a measurement it can only refuse.
    //
    // Its VALUE is load-bearing, though, and a comment here once said it decided
    // nothing. Every texel with no geometry behind it lands on it, which on a
    // scene with a black surround is a large fraction of the population sitting
    // far below everything else -- and it is why meter_low defaults as high as
    // it does. LUM_HISTOGRAM_MIN_LOG2 is this value, so that population lands in
    // bin 0 rather than below it; see the note there for what breaks otherwise.
    //
    // Both real bounds used to live here and both were ABSOLUTE cd/m^2. The
    // floor was the key itself -- texels darker than the key metered AS the key,
    // which made "auto only darkens" structural -- and the ceiling was 1e6,
    // stopping one sun pixel crushing the average. 11.52 replaced them with
    // percentile rejection in lum_reduce_frag, because an absolute threshold is
    // not scale-covariant: on a scene metering near the key, most of the frame
    // clamped UP and inflated the mean 3.05x, costing 1.61 stops between a scene
    // and the same scene at 1000x. A percentile over POPULATION survives that.
    //
    // The only-darkens invariant moved with it, to the fminf in
    // exposure_auto_gain, which is now the whole of it rather than a backstop.
    lum = max(lum, 1.0e-8);
    FragColor = vec4(log2(lum), 0.0, 0.0, 1.0);
}
