#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// TAAU: temporal upscale from the jittered render-res frame to post res.
// Deliberately a SEPARATE program from taa_resolve_frag, which stays the
// same-resolution resolve: sharing them would thread two-resolution logic
// through the path every full-scale frame runs, and the roadmap's guarantee
// is that scale 1 keeps that shader byte-identical.
uniform sampler2D currentTex;  // Render-res scene, rasterized with jitterPx applied
uniform sampler2D velocityTex; // Render-res motion vectors (.xy, UV units);
                               // fetched at an exact texel centre, so the
                               // filter mode never comes into it
uniform sampler2D historyTex;  // Post-res accumulated previous frames
uniform vec2 renderSize;
uniform vec2 postSize;
// This frame's jitter in render pixels. Subtracting it places each render
// sample where the un-jittered camera would have put it, which is what lets
// successive frames fill in different display pixels.
uniform vec2 jitterPx;
uniform int reset; // 1 on the very first frame: no valid history yet

// Same rest/motion window split as taa_resolve_frag (the rationale lives
// there); the moving window matters more here because a wrongly reprojected
// history is also carrying 1/scale^2 more of the image per texel.
const float TAA_FEEDBACK_REST = 0.97;
const float TAA_FEEDBACK_MOVING = 0.9;

#include "color.glsl"

// Sharp (bicubic) history sampling, at the history's own (post) resolution.
// Copied from taa_resolve_frag rather than hoisted to an include so that
// shader stays untouched (recorded follow-up).
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
    // This display pixel's position on the render raster, jitter un-applied.
    // MINUS, verified two ways: empirically (the flip test measured 34.2 dB
    // against 33.0 dB for +), and by the projection algebra -- a positive
    // [2][0] increment lands in x_ndc as -delta after the divide by w = -z,
    // so the raster shifted by -jitterPx and un-applying it subtracts.
    vec2 pos = uv * renderSize - jitterPx;
    // Centre of the render texel CONTAINING pos, so the 3x3 around it is
    // symmetric about pos and reaches every sample within 1.5 px on each side.
    // Anchoring on the nearest centre at or BELOW pos instead skews the window
    // one texel whenever pos sits in the lower half of its texel: the +1
    // neighbour (weight up to 0.10) drops out while a -2 one (weight under
    // 0.01) joins, and since the jitter moves pos every frame the skew -- in
    // the reconstruction AND in the clamp bounds gathered here -- flips per
    // frame per pixel. Measured: the skewed window churns 17% harder
    // frame-to-frame on a static camera. It also reads marginally sharper,
    // because dropping that neighbour drops kernel mass -- crispness at a
    // reduced scale is the sharpen pass's job, not a filter asymmetry's.
    vec2 base = floor(pos) + 0.5;
    // pos relative to that centre, |d| <= 0.5 per axis. base is therefore the
    // NEAREST sample, which is what makes the confidence and the velocity tap
    // below closed-form rather than a search through the loop.
    vec2 d0 = pos - base;

    // One loop over the 3x3 render sample centers does double duty:
    // Blackman-Harris-weighted reconstruction of this display pixel (the
    // Gaussian fit exp(-2.29 d^2), d in render px) and the YCoCg clamp bounds
    // (gathered in RENDER space -- wider in display terms below scale 1,
    // deliberately more ghost-tolerant).
    vec3 recon = vec3(0.0);
    float wsum = 0.0;
    vec3 nmin = vec3(1e9);
    vec3 nmax = vec3(-1e9);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 sp = base + vec2(float(x), float(y));
            vec2 suv = sp / renderSize; // exact texel center: bilinear = fetch
            vec3 c = texture(currentTex, suv).rgb;
            vec2 dv = sp - pos;
            float w = exp(-2.29 * dot(dv, dv));
            recon += c * w;
            wsum += w;
            vec3 n = rgbToYCoCg(c);
            nmin = min(nmin, n);
            nmax = max(nmax, n);
        }
    }
    // The centre tap alone weighs at least exp(-2.29 * 0.5) = 0.32, so wsum
    // needs no guard.
    vec3 current = recon / wsum;
    // Confidence: how close the nearest render sample landed to this display
    // pixel. Same weight the loop gave base, by the monotonicity of exp.
    float wmax = exp(-2.29 * dot(d0, d0));

    if (reset != 0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    // Reproject through the nearest render sample's velocity. The velocity
    // buffer is built from UN-jittered matrices, so it never needs the
    // jitter correction the color does.
    vec2 velocity = texture(velocityTex, base / renderSize).xy;
    vec2 histUv = uv - velocity;
    if (histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0) {
        FragColor = vec4(current, 1.0);
        return;
    }

    // Motion measured in display pixels: that is the grid the history lives on.
    float velPx = length(velocity * postSize);
    float feedback = mix(TAA_FEEDBACK_REST, TAA_FEEDBACK_MOVING, smoothstep(0.1, 1.0, velPx));

    vec3 history = sampleHistoryCatmullRom(historyTex, histUv, postSize);
    vec3 historyYCoCg = clamp(rgbToYCoCg(history), nmin, nmax);
    history = yCoCgToRgb(historyYCoCg);

    // Confidence-scaled blend: when no render sample landed near this display
    // pixel this frame (wmax small), the reconstruction is interpolation, not
    // information, so lean on history. The 0.25 floor keeps some inflow so a
    // never-sampled pixel still converges.
    float alpha = (1.0 - feedback) * mix(0.25, 1.0, wmax);

    // Inverse-luma weighting, taa_resolve_frag's firefly damping (the pivot
    // rationale lives there).
    vec3 currentYCoCg = rgbToYCoCg(current);
    float wCurrent = alpha / (1.0 + max(currentYCoCg.x, 0.0));
    float wHistory = (1.0 - alpha) / (1.0 + max(historyYCoCg.x, 0.0));
    FragColor = vec4((current * wCurrent + history * wHistory) / (wCurrent + wHistory), 1.0);
}
