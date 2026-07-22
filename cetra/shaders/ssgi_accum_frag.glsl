#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Temporal accumulation for SSGI -- an RGB mini-TAA on the half-res gathered
// irradiance. The GTAO/GI sweep jitters its slice directions per frame; this
// pass reprojects last frame's accumulated GI by the motion vectors and blends
// it with the current gather, integrating the jittered samples into a stable
// bounce (the raw per-frame gather is deliberately noisy). The neighborhood
// clamp runs in YCoCg so disocclusions bound luma without dragging chroma,
// and the blend is inverse-luma weighted so one bright gathered spark cannot
// accumulate into a persistent firefly (same policy as the TAA resolve).
// History is fetched BILINEAR on purpose (not the TAA resolve's Catmull-Rom):
// its negative lobes overshoot on HDR bounce light, and half-res GI has no
// sub-pixel detail worth preserving.
uniform sampler2D currentTex;  // This frame's raw gathered GI (.rgb, linear HDR)
uniform sampler2D velocityTex; // Screen-space motion .xy (UV units)
uniform sampler2D historyTex;  // Last frame's accumulated GI (.rgb)
uniform vec2 texelSize;        // 1 / GI resolution
uniform int reset;             // 1 on the first frame -> no history yet

const float FEEDBACK = 0.9; // History weight; ~10-frame effective window

#include "color.glsl"

void main()
{
    vec3 current = texture(currentTex, TexCoords).rgb;

    if (reset != 0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    vec2 velocity = texture(velocityTex, TexCoords).xy;
    vec2 histUv = TexCoords - velocity;
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        // Reprojected off-screen (disocclusion at the frame edge): current only.
        FragColor = vec4(current, 1.0);
        return;
    }

    // Bound the history to the current 3x3 YCoCg neighborhood so history from
    // a surface that just disoccluded can't bleed a stale bounce through.
    vec3 centerYCoCg = rgbToYCoCg(current);
    vec3 nmin = centerYCoCg;
    vec3 nmax = centerYCoCg;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue;
            vec3 n = rgbToYCoCg(
                texture(currentTex, TexCoords + vec2(float(x), float(y)) * texelSize).rgb);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    vec3 historyYCoCg = clamp(rgbToYCoCg(texture(historyTex, histUv).rgb), nmin, nmax);
    vec3 history = yCoCgToRgb(historyYCoCg);

    // Inverse-luma weighted blend (Y = YCoCg.x): resolves gathered fireflies
    // instead of hoarding them.
    float wCurrent = (1.0 - FEEDBACK) / (1.0 + max(centerYCoCg.x, 0.0));
    float wHistory = FEEDBACK / (1.0 + max(historyYCoCg.x, 0.0));
    FragColor = vec4((current * wCurrent + history * wHistory) / (wCurrent + wHistory), 1.0);
}
