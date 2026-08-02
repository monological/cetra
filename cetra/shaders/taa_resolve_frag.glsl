#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D currentTex;  // This frame's freshly resolved color (jittered)
uniform sampler2D velocityTex; // Screen-space motion vectors (.xy, UV units)
uniform sampler2D historyTex;  // Accumulated previous frame
uniform vec2 texelSize;        // 1/width, 1/height of the internal resolution

uniform int reset;             // 1 on the very first frame: no valid history yet

// History weight at REST vs IN MOTION. A static pixel's dominant per-frame
// variation is the TAA jitter itself -- exactly the signal to integrate --
// and the residual oscillation in the OUTPUT scales with (1 - feedback):
// at 0.9 (a ~10-frame window) detailed content like a sharp environment
// backdrop sizzles above the 8-bit threshold; 0.97 (~33 frames) pushes it
// under. The honest cost: LIGHTING changes on static geometry (a moved
// sun, a slider) converge ~3x slower too -- large steps are still snapped
// by the neighborhood clamp, but slow gradients inside the clamp box lag.
// In motion the window returns to the responsive value so parallax-wrong
// reprojection cannot smear (the 10.7.2 SSR accumulator's split).
const float TAA_FEEDBACK_REST = 0.97;
const float TAA_FEEDBACK_MOVING = 0.9;

// YCoCg separates luma from chroma, so the neighborhood clamp below bounds
// ghosting far better than clamping in RGB (a small luma change no longer drags
// the full colour). The transform is linear, so it is safe on linear HDR.
#include "color.glsl"

// Sharp (bicubic) history sampling. Plain bilinear reprojection smears the
// history a little every frame, which compounds into the classic TAA blur;
// Catmull-Rom keeps edges crisp for the cost of a handful of extra taps.
vec3 sampleHistoryCatmullRom(sampler2D tex, vec2 uv, vec2 res) {
    vec2 samplePos = uv * res;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0 = (texPos1 - 1.0) / res;
    vec2 texPos3 = (texPos1 + 2.0) / res;
    vec2 texPos12 = (texPos1 + offset12) / res;

    vec3 result = vec3(0.0);
    result += texture(tex, vec2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += texture(tex, vec2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += texture(tex, vec2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    result += texture(tex, vec2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += texture(tex, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += texture(tex, vec2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    result += texture(tex, vec2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += texture(tex, vec2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += texture(tex, vec2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    return result;
}

void main() {
    vec2 uv = TexCoords;
    vec3 current = texture(currentTex, uv).rgb;

    if (reset != 0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    // Reproject: read history from where this pixel was last frame.
    vec2 velocity = texture(velocityTex, uv).xy;
    vec2 histUv = uv - velocity;

    // Motion measure for the adaptive window, from the SAME vector that
    // reprojects the history -- that identity is what makes "in motion,
    // distrust history" sound. velPx is pixels-per-frame at the INTERNAL
    // resolution (not effect resolution like ssr_accum's): below 0.1 px is
    // the dead-band for subpixel reprojection noise, 1 px and up is full
    // motion.
    float velPx = length(velocity / texelSize);
    float feedback = mix(TAA_FEEDBACK_REST, TAA_FEEDBACK_MOVING, smoothstep(0.1, 1.0, velPx));

    // Reprojected off-screen (disocclusion / camera turned onto new content):
    // there is no valid history, so fall back to the current frame.
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    vec3 history = sampleHistoryCatmullRom(historyTex, histUv, 1.0 / texelSize);

    // Neighborhood clamp: bound the history to the 3x3 YCoCg min/max of the
    // current frame. Moving edges and disoccluded pixels whose history falls
    // outside this box get clamped to plausible current colour instead of
    // ghosting or trailing.
    vec3 centerYCoCg = rgbToYCoCg(current);
    vec3 nmin = centerYCoCg;
    vec3 nmax = centerYCoCg;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0)
                continue;
            vec3 n = rgbToYCoCg(texture(currentTex, uv + vec2(float(x), float(y)) * texelSize).rgb);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    vec3 historyYCoCg = clamp(rgbToYCoCg(history), nmin, nmax);
    history = yCoCgToRgb(historyYCoCg);

    // Blend, weighted by inverse luminance. A plain lerp lets a bright specular
    // spark in either the current frame or the reprojected history survive the
    // neighborhood clamp and accumulate into a field of sparkle on
    // high-frequency surfaces (scratched metal). Down-weighting the brighter
    // sample lets the temporal average actually resolve those fireflies — the
    // standard fix for TAA on aliasing-prone specular. Y (YCoCg.x) is luma.
    //
    // The 1.0 in the denominator is a pivot, not a clamp: it is the luma at
    // which down-weighting reaches half, and it is deliberately left absolute
    // because in working space 1.0 IS diffuse white (view.glsl). So the filter
    // pushes back on anything brighter than white and leaves the rest alone,
    // which is the intent. Left as a bare 1.0 rather than a named constant for
    // the same reason — it is the contract's own unit, and naming it would
    // suggest it is tunable independently of what white means.
    float wCurrent = (1.0 - feedback) / (1.0 + max(centerYCoCg.x, 0.0));
    float wHistory = feedback / (1.0 + max(historyYCoCg.x, 0.0));
    FragColor = vec4((current * wCurrent + history * wHistory) / (wCurrent + wHistory), 1.0);
}
