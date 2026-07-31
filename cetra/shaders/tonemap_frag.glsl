#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Final post pass: composite bloom onto the linear HDR scene, then apply
// exposure, ACES tone mapping, and gamma. Mode 0 is a raw copy for frames
// that are already display-ready (debug render modes, LDR-authored apps).
// hdrTex arrives pre-exposed; this pass reads it in working space and only
// applies the residual EV bias, so WS_SCENE_MAX below is the same ceiling the
// shading passes wrote under.
#include "view.glsl"

uniform sampler2D hdrTex;
uniform sampler2D bloomTex;
uniform sampler2D aoTex; // Blurred SSAO, upsampled by its linear filter
uniform float bloomStrength;
uniform int bloomEnabled;
uniform int aoEnabled;
uniform float aoStrength;
uniform sampler2D normalsTex; // Resolved view-space normal .xyz + SSR marker .a
uniform sampler2D ssrTex;     // Half-res reflection buffer
uniform sampler2D albedoTex;  // Resolved albedo G-buffer (SSGI)
uniform sampler2D giTex;      // Half-res gathered GI radiance (SSGI)
// Specular occlusion: GTAO approximates DIFFUSE occlusion, so multiplying it
// onto specular/reflections is wrong -- it darkens (and, at silhouettes,
// shimmers) a bright grazing specular. These recover the reflection: aux .z/.w
// give view-Z + roughness, normalsTex the view normal, for a Lagarde term.
uniform sampler2D auxTex;       // Aux G-buffer: linear view-Z (.z) + roughness (.w)
uniform vec2 invFocal;          // 1/projection focal terms, for view-pos reconstruction
uniform int specOccEnabled;     // Apply specular occlusion to the AO factor
uniform int specOccHasMetallic; // albedoTex.a carries metallic this frame (SSGI wrote it)
uniform sampler2D csTex;        // Contact-shadow visibility (spec 9.3), AO res
uniform int csEnabled;          // Multiply the direct-light term by contact shadows
uniform float csStrength;       // Contact-shadow darkening weight
// Debug view dispatch (PostFXDebugView): 0=none, 1=AO, 2=normals, 3=SSR,
// 4=albedo, 5=GI, 6=fog, 7=spec-occ AO, 8=contact shadows
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

    // Weight toward white, and the leading 1.0 - is load-bearing. Without it the
    // blend inverts: slight compression (peak just over startCompression, so
    // peak - newPeak near 0) would desaturate ~fully instead of not at all, and
    // the curve stops being a roll-off and becomes a hard clip at 0.76 -- every
    // highlight snapping to a flat white plateau while still looking, in code,
    // like tonemapping.
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, newPeak * vec3(1.0), g);
}

// 6th-order polynomial fit of the AgX base sigmoid
vec3 agxContrastApprox(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
           0.1191 * x - 0.00232;
}

// AgX (Sobotka; Blender 4+'s default view transform), Wrensch's fitted
// minification. Desaturates toward white as radiance climbs, so saturated
// HDR highlights roll off without the hue skew ACES produces. The inset/
// outset matrices and sigmoid coefficients are the published constants and
// are inverses of each other through the curve's 2.2 encoding -- transcribe
// exactly or everything takes a global color cast.
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
    const float INV_EV_RANGE = 1.0 / (MAX_EV - MIN_EV);

    c = AGX_INSET * c;
    // log2 of black is -inf; the floor keeps the clamp finite
    c = clamp(log2(max(c, vec3(1e-10))), MIN_EV, MAX_EV);
    c = (c - MIN_EV) * INV_EV_RANGE;
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
// HDR debug views. Returns LDR-linear [0,1]; displayEncode gammas later.
vec3 toneSelect(vec3 c)
{
    if (tonemapMode == 1)
        return acesTonemap(c);
    if (tonemapMode == 3)
        return agxTonemap(c);
    return pbrNeutralTonemap(c);
}

// Screen-space AO visibility with specular occlusion. GTAO measures DIFFUSE
// hemispherical occlusion; a smooth reflection instead sees the environment in
// its mirror direction, past the local occluders GTAO sampled -- so it must not
// be dimmed (nor jittered) by that AO. We blend the AO toward unoccluded (1.0)
// by the pixel's specular fraction x smoothness: a smooth grazing specular (high
// dielectric Fresnel or a metal, low roughness) goes fully unoccluded, killing
// the AO's silhouette shimmer off it; diffuse/rough pixels keep the plain AO.
// Off (specOccEnabled 0) returns raw ao -> byte-identical to the pre-feature path.
float aoVisibility()
{
    float ao = texture(aoTex, TexCoords).r;
    if (specOccEnabled == 0)
        return ao;
    vec4 nrm = texture(normalsTex, TexCoords);
    // Real model surfaces only: a non-zero normal excludes sky/hair (zero
    // marker), and a > -0.5 excludes the shadow-catcher floor (a = -1).
    if (dot(nrm.xyz, nrm.xyz) < 0.01 || nrm.a <= -0.5)
        return ao;
    vec4 aux = texture(auxTex, TexCoords); // .w = effective roughness
    // View direction from screen UV. normalize(-viewPos) is independent of depth
    // (same direction everywhere along a camera ray), so linZ cancels -- no need
    // to reconstruct the view position, just the ray direction from NDC + focal.
    vec2 ndc = TexCoords * 2.0 - 1.0;
    vec3 V = normalize(vec3(-ndc * invFocal, 1.0));
    float NdotV = clamp(dot(normalize(nrm.xyz), V), 0.0, 1.0);
    float metallic = specOccHasMetallic == 1 ? texture(albedoTex, TexCoords).a : 0.0;
    float fresnel = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0); // dielectric specular fraction
    float specWeight = mix(fresnel, 1.0, metallic) * (1.0 - aux.w); // x mirror-ness
    return mix(ao, 1.0, specWeight);
}

// Scene HDR sample -> tonemapped LDR-linear [0,1], applying the shared AO
// factor and bloom addition (the same order the composite uses). Sharpen
// neighbour taps reuse the centre's aoFactor/bloomAdd so the mask measures
// scene edges, not AO/bloom gradients.
//
// No exposure here. The buffer arrives fully exposed -- camera AND adaptation
// both, applied at the scene passes (view.glsl) -- so this pass only maps
// working space to display.
vec3 sceneToToned(vec3 hdr, float aoFactor, vec3 bloomAdd)
{
    // Sanitize a +INF texel (half-float overflow upstream) — both tonemap
    // curves turn INF into NaN, which displays as a black pixel
    vec3 c = min(hdr, vec3(WS_SCENE_MAX)) * aoFactor + bloomAdd;
    return toneSelect(c);
}

void main()
{
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
    // 6 was fog in-scatter, retired with the screen-space march (spec 9.5):
    // the froxel volume composites directly and has no 2D buffer to show.
    if (debugView == 7) {
        // Spec-occ AO visibility (what actually multiplies the scene) -- the
        // reflection relief vs the raw AO of debug view 1.
        FragColor = vec4(vec3(aoVisibility()), 1.0);
        return;
    }
    if (debugView == 8) {
        // Contact-shadow visibility term before compositing (1 = lit)
        FragColor = vec4(vec3(texture(csTex, TexCoords).r), 1.0);
        return;
    }

    // Occlude before adding bloom: bloom models lens scatter, which happens
    // after the light already left the scene
    float aoFactor = 1.0;
    if (aoEnabled == 1)
        aoFactor = mix(1.0, aoVisibility(), aoStrength);
    // Contact shadows fold into the same factor (so the sharpen taps inherit
    // them like AO) but stay independent of AO/spec-occ: they occlude direct
    // light, not ambient. Exact identity at cs == 1, so a lit frame matches the
    // feature off bit for bit.
    if (csEnabled == 1)
        aoFactor *= 1.0 - csStrength * (1.0 - texture(csTex, TexCoords).r);
    vec3 bloomAdd = vec3(0.0);
    if (bloomEnabled == 1)
        bloomAdd = bloomStrength * texture(bloomTex, TexCoords).rgb;

    vec3 color = sceneToToned(texture(hdrTex, TexCoords).rgb, aoFactor, bloomAdd);

    // Sharpen: unsharp mask on the tonemapped result (4-tap cross)
    if (sharpenEnabled == 1) {
        vec3 blur = sceneToToned(texture(hdrTex, TexCoords + vec2(texelSize.x, 0.0)).rgb, aoFactor,
                                 bloomAdd) +
                    sceneToToned(texture(hdrTex, TexCoords - vec2(texelSize.x, 0.0)).rgb, aoFactor,
                                 bloomAdd) +
                    sceneToToned(texture(hdrTex, TexCoords + vec2(0.0, texelSize.y)).rgb, aoFactor,
                                 bloomAdd) +
                    sceneToToned(texture(hdrTex, TexCoords - vec2(0.0, texelSize.y)).rgb, aoFactor,
                                 bloomAdd);
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
