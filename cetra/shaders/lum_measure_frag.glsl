#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Auto-exposure step 1: down-measure the linear HDR scene into a small target
// storing log2 luminance. Mipmapping this target afterwards averages the logs,
// so the top mip holds the scene's geometric-mean (photographic) luminance --
// robust to a few very bright pixels, unlike an arithmetic mean.
#include "view.glsl"

uniform sampler2D hdrTex; // Resolved linear HDR scene
uniform float autoKey;    // Middle-gray target; ALSO the metering floor (see below)

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
    // Clamp the metered range so extremes can't hijack the average. The floor
    // is the key itself: texels darker than the key meter AS the key, so the
    // mean can never fall below it and the auto gain (key / mean) tops out at
    // 1x -- auto-exposure only ever DARKENS an over-bright scene. Boosting
    // dark scenes is what it must not do: a subject framed against black void
    // (no environment) would otherwise meter low and blow out. Sharing the
    // autoKey uniform with the tonemap makes that invariant structural. The
    // ceiling stops one sun pixel from crushing everything else.
    lum = clamp(lum, autoKey, 10000.0);
    FragColor = vec4(log2(lum), 0.0, 0.0, 1.0);
}
