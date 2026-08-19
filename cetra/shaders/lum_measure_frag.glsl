#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Auto-exposure step 1: down-measure the linear HDR scene into a small target
// storing log2 luminance. Mipmapping this target afterwards averages the logs,
// so the top mip holds the scene's geometric-mean (photographic) luminance --
// robust to a few very bright pixels, unlike an arithmetic mean.
#include "view.glsl"

uniform sampler2D hdrTex; // Resolved linear HDR scene

void main()
{
    // Back to absolute scene radiance first. The buffer is pre-exposed
    // (view.glsl), and metering it in working space would measure this pass's
    // own contribution to the exposure that produced it -- a feedback loop that
    // ratchets toward the clamp instead of settling. autoKey below is an
    // absolute middle grey and only means anything against absolute input.
    // Sanitize BEFORE converting: a +INF texel would poison the whole average
    // through the mip chain, and the ceiling is the one pbr_frag already wrote
    // under, so it clips nothing a shading pass let through. Applying it after
    // the conversion would instead cap absolute radiance at 60000, which a noon
    // sun legitimately exceeds -- the meter would read it as overcast.
    vec3 hdr = min(texture(hdrTex, TexCoords).rgb, vec3(WS_SCENE_MAX)) * oneOverPreExposure;
    float lum = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    // A NUMERIC guard, and nothing else. log2(0) is -INF, which would poison the
    // histogram's sum and reach the CPU as a measurement it can only refuse; the
    // value is twenty-odd stops below anything a scene contains, so it decides
    // nothing about metering.
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
