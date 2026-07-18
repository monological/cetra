#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Final post pass: composite bloom onto the linear HDR scene, then apply
// exposure, ACES tone mapping, and gamma. Mode 0 is a raw copy for frames
// that are already display-ready (debug render modes, LDR-authored apps).
uniform sampler2D hdrTex;
uniform sampler2D bloomTex;
uniform sampler2D aoTex; // Blurred SSAO, upsampled by its linear filter
uniform float exposure;
uniform float bloomStrength;
uniform int bloomEnabled;
uniform int aoEnabled;
uniform float aoStrength;
uniform sampler2D normalsTex; // Resolved view-space normals + roughness
uniform sampler2D ssrTex;     // Half-res reflection buffer
uniform sampler2D albedoTex;  // Resolved albedo G-buffer (SSGI)
uniform sampler2D giTex;      // Half-res gathered GI radiance (SSGI)
// Auto-exposure: scale exposure by key / adapted average luminance, so the
// scene's mean lands near photographic middle gray. The manual exposure
// uniform then acts as an EV bias on top.
uniform sampler2D lumTex; // 1x1 adapted log2 mean luminance
uniform sampler2D fogTex; // Half-res fog in-scatter (debug view)
uniform int autoExposure;
uniform float autoKey; // Target middle gray (0.18)
// Debug view dispatch (PostFXDebugView): 0=none, 1=AO, 2=normals, 3=SSR,
// 4=albedo, 5=GI, 6=fog
uniform int debugView;
// 1 = ACES, 2 = PBR Neutral, 3 = AgX (passthrough frames are blitted by
// postfx_run and never reach this pass)
uniform int tonemapMode;
uniform vec2 texelSize; // Display-pixel size, for the sharpen taps

// Finishing grade — a "look" stack applied after tone mapping. Each stage is
// gated by its own toggle; with all off the output is the plain tonemapped
// frame. Order: sharpen -> grade -> vignette -> gamma -> grain.
uniform int sharpenEnabled;
uniform float sharpenStrength;
uniform int gradeEnabled;
uniform vec3 gradeLift;  // Black point / shadow raise (0 = none)
uniform vec3 gradeGamma; // Per-channel midtone curve (1 = none)
uniform vec3 gradeGain;  // White point / highlight scale (1 = none)
uniform int vignetteEnabled;
uniform float vignetteStrength;
uniform float vignetteRadius;
uniform int grainEnabled;
uniform float grainStrength;
uniform float grainSeed; // Per-frame, deterministic across equal --frames runs

// ACES filmic fit (Narkowicz 2015). High contrast: crushes shadows,
// desaturates highlights — the cinematic look.
vec3 acesTonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

// Khronos PBR Neutral (2024). Identity below ~0.76 so shadows, midtones,
// and material colors stay faithful; only highlights are compressed.
// Made for product/model viewers where albedo fidelity matters.
vec3 pbrNeutralTonemap(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

// AgX (Sobotka; Blender 4+'s default view transform), Wrensch's fitted
// minification. Desaturates toward white as radiance climbs, so saturated
// HDR highlights roll off without the hue skew ACES produces. The inset/
// outset matrices and sigmoid coefficients are the published constants and
// are inverses of each other through the curve's 2.2 encoding -- transcribe
// exactly or everything takes a global color cast.
vec3 agxContrastApprox(vec3 x)
{
    // 6th-order polynomial fit of the AgX base sigmoid
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
           0.1191 * x - 0.00232;
}

vec3 agxTonemap(vec3 c)
{
    const mat3 AGX_INSET = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                0.0784335999999992, 0.878468636469772, 0.0784336,
                                0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 AGX_OUTSET = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                 -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                 -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float MIN_EV = -12.47393;
    const float MAX_EV = 4.026069;

    c = AGX_INSET * c;
    // log2 of black is -inf; the floor keeps the clamp finite
    c = clamp(log2(max(c, vec3(1e-10))), MIN_EV, MAX_EV);
    c = (c - MIN_EV) / (MAX_EV - MIN_EV);
    c = agxContrastApprox(c); // output is 2.2-encoded by construction
    c = AGX_OUTSET * c;
    // Linearize: toneSelect's contract is LDR-linear, displayEncode does
    // the gamma later
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// Gamma-encode a linear [0,1] color for display.
vec3 displayEncode(vec3 c)
{
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

// The frame's selected tonemap curve, shared by the scene path and the
// HDR debug views.
vec3 toneSelect(vec3 c)
{
    return tonemapMode == 3   ? agxTonemap(c)
           : tonemapMode == 1 ? acesTonemap(c)
                              : pbrNeutralTonemap(c);
}

// Scene HDR sample -> tonemapped LDR-linear [0,1], applying the shared AO
// factor and bloom addition (the same order the composite uses). Sharpen
// neighbour taps reuse the centre's aoFactor/bloomAdd so the mask measures
// scene edges, not AO/bloom gradients. effExposure is the frame's effective
// exposure (manual, or auto x manual bias), computed once in main().
vec3 sceneToToned(vec3 hdr, float aoFactor, vec3 bloomAdd, float effExposure)
{
    // Sanitize a +INF texel (half-float overflow upstream) — both tonemap
    // curves turn INF into NaN, which displays as a black pixel
    vec3 c = min(hdr, vec3(60000.0)) * aoFactor + bloomAdd;
    c *= effExposure;
    return toneSelect(c);
}

void main()
{
    float effExposure = exposure;
    if (autoExposure == 1) {
        // The metering floor equals the key (enforced structurally: the
        // measure pass clamps with the same autoKey uniform), so avgLum >=
        // autoKey and the gain tops out at 1 -- auto-exposure only darkens.
        float avgLum = exp2(texture(lumTex, vec2(0.5)).r);
        effExposure = exposure * clamp(autoKey / avgLum, 1.0 / 64.0, 1.0);
    }
    if (debugView == 1) {
        FragColor = vec4(vec3(texture(aoTex, TexCoords).r), 1.0);
        return;
    }
    if (debugView == 2) {
        // Remap to display range; unwritten texels (sky, hair) stay black
        vec3 n = texture(normalsTex, TexCoords).xyz;
        FragColor = vec4(dot(n, n) > 0.001 ? normalize(n) * 0.5 + 0.5 : vec3(0.0), 1.0);
        return;
    }
    if (debugView == 3) {
        // Raw reflection buffer, gamma-corrected so dim hits are visible
        FragColor = vec4(displayEncode(texture(ssrTex, TexCoords).rgb), 1.0);
        return;
    }
    if (debugView == 4) {
        // Albedo G-buffer (stored linear); gamma-encode for display
        FragColor = vec4(displayEncode(texture(albedoTex, TexCoords).rgb), 1.0);
        return;
    }
    if (debugView == 5) {
        // Gathered GI radiance (linear HDR); tone map so bright bounces
        // don't clip to white, gamma-encode for display
        vec3 gi = toneSelect(texture(giTex, TexCoords).rgb);
        FragColor = vec4(displayEncode(gi), 1.0);
        return;
    }
    if (debugView == 6) {
        // Fog in-scatter (linear HDR); tone map so bright shafts read
        vec3 fog = toneSelect(texture(fogTex, TexCoords).rgb);
        FragColor = vec4(displayEncode(fog), 1.0);
        return;
    }

    // Occlude before adding bloom: bloom models lens scatter, which happens
    // after the light already left the scene
    float aoFactor = 1.0;
    if (aoEnabled == 1)
        aoFactor = mix(1.0, texture(aoTex, TexCoords).r, aoStrength);
    vec3 bloomAdd = vec3(0.0);
    if (bloomEnabled == 1)
        bloomAdd = bloomStrength * texture(bloomTex, TexCoords).rgb;

    vec3 color = sceneToToned(texture(hdrTex, TexCoords).rgb, aoFactor, bloomAdd, effExposure);

    // Sharpen: unsharp mask on the tonemapped result (4-tap cross)
    if (sharpenEnabled == 1) {
        vec3 blur = sceneToToned(texture(hdrTex, TexCoords + vec2(texelSize.x, 0.0)).rgb, aoFactor,
                                 bloomAdd, effExposure) +
                    sceneToToned(texture(hdrTex, TexCoords - vec2(texelSize.x, 0.0)).rgb, aoFactor,
                                 bloomAdd, effExposure) +
                    sceneToToned(texture(hdrTex, TexCoords + vec2(0.0, texelSize.y)).rgb, aoFactor,
                                 bloomAdd, effExposure) +
                    sceneToToned(texture(hdrTex, TexCoords - vec2(0.0, texelSize.y)).rgb, aoFactor,
                                 bloomAdd, effExposure);
        color = clamp(color + sharpenStrength * (color - blur * 0.25), 0.0, 1.0);
    }

    // Colour grade: lift/gamma/gain (identity at the defaults)
    if (gradeEnabled == 1) {
        color = gradeLift + color * (gradeGain - gradeLift);
        color = pow(max(color, 0.0), 1.0 / gradeGamma);
    }

    // Vignette: radial edge darkening. 0.7071 is the centre-to-corner UV
    // distance, so radius is the fraction of that kept fully bright.
    if (vignetteEnabled == 1) {
        float d = length(TexCoords - 0.5);
        float falloff = smoothstep(vignetteRadius * 0.7071, 0.7071, d);
        color *= 1.0 - vignetteStrength * falloff;
    }

    // Gamma-encode to display space
    color = displayEncode(color);

    // Film grain: display-space, weighted toward midtones (invisible in flat
    // black/white), animated by a deterministic per-frame seed
    if (grainEnabled == 1) {
        float n =
            fract(sin(dot(gl_FragCoord.xy + grainSeed, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
        float luma = dot(color, vec3(0.299, 0.587, 0.114));
        float w = 1.0 - abs(2.0 * luma - 1.0);
        color = clamp(color + n * grainStrength * w, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
