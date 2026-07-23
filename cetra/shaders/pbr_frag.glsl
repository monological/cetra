#version 330 core
in vec3 Normal;
in vec3 WorldPos;
in vec3 ViewPos;
in vec2 TexCoords;
in vec2 TexCoords2;   // UV1 for lightmaps/AO
in vec4 VertexColor;  // Vertex color (RGBA)
in mat3 TBN;
// Bitangent handedness (tangent.w), flat because it is constant per UV island
// -- interpolating across a mirror seam would blend +1 toward -1.
flat in float TangentW;
in vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
in vec4 PrevClip;     // Previous-frame clip position
layout(location = 0) out vec4 FragColor;
// G-buffer for screen-space passes: view-space normal (xyz) + roughness (a).
// Only lands when the engine enables color attachment 1; otherwise discarded.
layout(location = 1) out vec4 NormalOut;
// Screen-space motion vector (.xy, UV units) for TAA. Only lands when the
// engine enables color attachment 2 (TAA active); otherwise discarded.
layout(location = 2) out vec4 VelocityOut;
// SSGI G-buffer: linear base color (.rgb) + metalness (.a) for the one-bounce
// composite (indirect diffuse = (1-metallic) * albedo * gathered irradiance, so
// metals get no bounced diffuse). Only lands when attachment 3 is enabled (SSGI).
layout(location = 3) out vec4 AlbedoOut;
// SSS diffuse: skin diffuse irradiance * subsurface, 0 off-skin (so it
// doubles as the mask). The SSS post pass blurs this and composites
// hdr + blur - this, softening diffuse while FragColor's specular stays sharp.
// Only lands when attachment 4 is enabled (sssEnabled); otherwise discarded.
layout(location = 4) out vec4 DiffuseOut;
// Weighted-blended OIT (only bound during the OIT accumulate pass, oitPass > 0):
// AccumOut = premultiplied color * depth weight, RevealageOut = alpha (.r).
layout(location = 5) out vec4 AccumOut;
layout(location = 6) out vec4 RevealageOut;

uniform mat4 view;
uniform mat4 projection;

uniform int renderMode;

uniform vec3 albedo;
uniform vec3 emissiveFactor;  // Emissive color factor (multiplied with emissive texture)
uniform float metallic;
uniform float roughness;
uniform float ao;
uniform float materialOpacity;
uniform float alphaCutoff;  // Alpha cutoff threshold for hair/foliage (0 = disabled)
// Alpha-to-coverage: MSAA turns fractional alpha into sample coverage, so the
// binary cutoff is skipped for soft, order-independent masked edges
uniform int alphaToCoverage;
// Whether this material is ALPHA_MASK at all. Distinct from alphaToCoverage,
// which says only that A2C is live for this draw: A2C needs MSAA samples to
// dither into and switches off on a 1-sample buffer, but the material is still
// masked, and the shadow/GTAO rules below key off the material, not the AA mode.
uniform int alphaMasked;
// Masked material that opted back into the shadow map (material.h
// foliage_shadows). Leaf cards are large enough to resolve at map-texel scale,
// so they read the cascades like opaque geometry does.
uniform int foliageShadows;
// Geometric specular AA strength (0 disables)
uniform float specularAAStrength;

uniform float normalScale;  // Normal map intensity scale (1.0 = full strength)
uniform float aoStrength;   // Occlusion texture strength (1.0 = full effect)
uniform float ior;
// KHR_materials_transmission: 0 = opaque; > 0 replaces the diffuse term
// with the resolved opaque scene color sampled through the surface
uniform float transmission;
uniform float transmissionThickness; // KHR_materials_volume, world units
uniform sampler2D sceneColorTex;     // Mipped opaque-scene resolve (unit 6)
uniform int sceneColorAvailable;     // 1 only in the late pass after the resolve
// Coarsest blur mip the transmission sample may select (the resolve's mip
// generation stops here too -- keep in step with OPAQUE_COLOR_MAX_LOD)
const float TRANSMISSION_MAX_LOD = 6.0;
uniform float filmThickness;
uniform float clearcoat;          // KHR_materials_clearcoat weight (0 = no coat lobe)
uniform float clearcoatRoughness; // coat lobe roughness
uniform float specularFactor;     // KHR_materials_specular weight (-1 = extension absent)
uniform vec3 specularColorFactor; // KHR_materials_specular F0 tint (white = no tint)
uniform vec3 sheenColorFactor;      // KHR_materials_sheen color ((0,0,0) = no sheen lobe)
uniform float sheenRoughnessFactor; // KHR_materials_sheen roughness
uniform float parallaxScale;        // POM march depth in UV units (0 = off, §4.11)
uniform vec2 uvOffset;      // Texture coordinate offset (KHR_texture_transform)
uniform vec2 uvScale;       // Texture coordinate scale (KHR_texture_transform)
uniform float uvRotation;   // Texture coordinate rotation in radians
uniform int vertexColorExists;  // Whether mesh has vertex colors
uniform int texCoords2Exists;   // Whether mesh has UV1
// Which displacement model this material uses (material.h wind_mode). Read here
// only to know what UV1 MEANS: the vegetation modes redefine it as wind data
// (branch phase, flex weight) rather than a texture coordinate set, so the AO
// path below must not sample a map with it. Shares its location with the
// vertex stage's declaration; the existing per-material upload feeds both.
uniform int uWindMode;
uniform vec3 camPos;

uniform sampler2D albedoTex;
uniform sampler2D normalTex;
uniform sampler2D emissiveTex;
uniform sampler2D sheenTex;          // KHR sheen color (sRGB, unit 8)
uniform sampler2D clearcoatNormalTex; // clearcoat normal map (freed unit)
uniform sampler2D heightTex;          // POM height field (unit 4, §4.11); white = raised

// The scalar masks (roughness/metallic/ao/opacity/microsurface/anisotropy/
// subsurface) share ONE array texture. Each material selects a layer per mask;
// a layer < 0 means no texture -> fall back to the scalar factor. roughness
// reads .g and metallic reads .b (glTF ORM); the rest read .r.
uniform sampler2DArray maskArray;
uniform int roughnessLayer;
uniform int metallicLayer;
uniform int aoLayer;
uniform int opacityLayer;
uniform int microsurfaceLayer;
uniform int anisotropyLayer;

uniform int albedoTexExists;
uniform int normalTexExists;
uniform int emissiveTexExists;
uniform int heightTexExists;
uniform int sheenTexExists;
uniform int clearcoatNormalExists;

// Shadow mapping. The shared block (maps, matrices, cascade params, bias,
// texel size) comes from the chunk; everything below is specific to this
// shader's per-fragment cascade selection and PCSS, which the catcher and
// particle consumers do not do. No CSM_OUTERMOST_PCF: they sample one
// scene-fit cascade, this one selects per fragment.
#include "csm.glsl"

uniform vec4 cascadeSplits; // View-depth far bound per cascade (.xyz)
uniform int csmDebug; // Tint fragments by selected cascade
// (no numShadowLights here: each DirLight carries its own CSM slot.
// shadow.c still uploads it -- catcher_frag and particle_frag do read it.)
// PCSS (contact-hardening shadows). When disabled the 3x3 PCF fallback
// runs, bit-identical to the pre-PCSS path. The ortho shadow projection
// stores depth linearly in [near,far], so the blocker/receiver separation
// that sets the penumbra width is measured on linearized depths; the
// per-cascade ortho geometry comes from cascadeParams.
uniform int pcssEnabled;
uniform float pcssSoftness; // Multiplier on the light's angular size

// Perspective spot shadow map (the flashlight): occludes surfaces (e.g. the
// glass ball's shadow on the floor). Separate from the directional cascade
// array; perspective, so its tap needs a per-fragment w-divide.
uniform sampler2D spotShadowMap;
uniform mat4 spotShadowMatrix;
uniform int spotShadowActive;

// Clustered-forward light blocks (spec 9.1): all analytic lights arrive
// through these UBOs -- directionals in a small unconditional array, the
// point/spot/area set via the per-fragment cluster index list.
#include "lights_ubo.glsl"

uniform int clusterDebug; // Tint fragments by cluster light count (heatmap)

// Cascade for a view depth: the first cascade whose far bound contains it.
// At cascadeCount 1 the loop never runs (cascade 0).
int selectCascade(float viewDepth)
{
    int cascade = 0;
    for (int c = 0; c < cascadeCount - 1; c++) {
        if (viewDepth > cascadeSplits[c])
            cascade = c + 1;
    }
    return cascade;
}

// IBL (Image-Based Lighting) uniforms
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;
// Multi-scatter energy compensation toggle (inert unless iblEnabled)
uniform int energyCompEnabled;
uniform int clearcoatEnabled; // Global clearcoat lobe toggle (--no-clearcoat)
uniform int specularEnabled;  // Global KHR_materials_specular toggle (--no-specular)
uniform int sheenEnabled;     // Global KHR_materials_sheen toggle (--no-sheen)
uniform int parallaxEnabled;  // Global POM toggle (--no-parallax, §4.11)
uniform int oitPass;          // 1 during the weighted-blended OIT accumulate pass (else 0)
uniform int sssEnabled;       // Global separable-SSS toggle (--no-sss)
uniform float subsurface;     // Per-material SSS strength (0 = off; also the skin flag)
uniform int sssProfileIndex;  // This material's scatter-profile slot; written into DiffuseOut.a so
                              // the screen-space blur can look up the profile per pixel
uniform vec3 subsurfaceColor; // Scatter tint; here it colors the back-light transmission (the
                              // front-scatter radius/color live on the global SSS blur pass)

// Local reflection probe: the scene captured into a prefiltered cubemap,
// parallax-corrected against a proxy AABB (Lagarde 2012). When enabled, the
// probe is bound INTO the prefilteredMap slot (the fragment stage is already
// at the driver's sampler limit, so no extra samplerCube) and replaces the
// infinite-distance global environment for specular.
uniform int probeEnabled;
uniform vec3 probePos;
uniform vec3 probeBoxMin;
uniform vec3 probeBoxMax;
uniform float probeIntensity;
uniform float probeMaxLOD;
uniform float probeBoxFade;

const float PI = 3.14159265359;

// With alpha-to-coverage active only fully invisible fragments are discarded
// (fractional alpha becomes MSAA coverage); otherwise the binary cutoff
// applies (alphaCutoff of 0 never discards since alpha is non-negative)
const float A2C_MIN_ALPHA = 0.02;

bool alphaBelowCutoff(float a)
{
    return a < (alphaToCoverage > 0 ? A2C_MIN_ALPHA : alphaCutoff);
}

// UV transform for KHR_texture_transform
vec2 transformUV(vec2 uv) {
    // Apply rotation around origin
    float s = sin(uvRotation);
    float c = cos(uvRotation);
    vec2 rotated = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    // Apply scale and offset
    return rotated * uvScale + uvOffset;
}

// Parallax Occlusion Mapping (§4.11): march the height field along the
// tangent-space view direction Vts and return the UV where the ray first dips
// below the surface, so a flat plane fakes real relief with self-occlusion.
// Height convention: depth = 1 - height (white = raised). Adaptive step count
// (more steps at grazing, where the lateral offset is longest and aliases
// worst), then interpolate the last two samples for a sub-step-accurate hit
// (the "occlusion" in POM). Vts.z is floored so the offset cannot blow up as the
// view grazes the surface.
vec2 parallaxOcclusion(vec2 uv0, vec3 Vts) {
    const float MAX_LAYERS = 32.0;
    const float MIN_LAYERS = 8.0;
    float numLayers = mix(MAX_LAYERS, MIN_LAYERS, clamp(abs(Vts.z), 0.0, 1.0));
    float layerDepth = 1.0 / numLayers;
    // Per-step UV offset: the deepest layer shifts by (Vts.xy / Vts.z) * scale.
    vec2 dUV = (Vts.xy / max(abs(Vts.z), 0.1)) * parallaxScale / numLayers;

    float curDepth = 0.0;
    vec2 curUV = uv0;
    float curH = 1.0 - texture(heightTex, curUV).r;
    for (int i = 0; i < int(MAX_LAYERS); i++) {
        if (curDepth >= curH)
            break;
        curUV -= dUV;
        curH = 1.0 - texture(heightTex, curUV).r;
        curDepth += layerDepth;
    }
    // Interpolate between the current (under the surface) and previous (over) tap.
    vec2 prevUV = curUV + dUV;
    float afterD = curH - curDepth;                                                 // <= 0
    float beforeD = (1.0 - texture(heightTex, prevUV).r) - (curDepth - layerDepth); // >= 0
    // Guard the degenerate flat-plateau crossing (afterD == beforeD == 0 -> 0/0 -> NaN).
    float denom = afterD - beforeD;
    float w = abs(denom) > 1e-6 ? afterD / denom : 1.0;
    return mix(curUV, prevUV, clamp(w, 0.0, 1.0));
}

// Soft POM self-shadow (§4.11 M4): march the height field from the resolved hit
// (uv, h0 = height there) toward the tangent-space light direction Lts; relief
// between the point and the light occludes it, darkening the grooves. Returns 1
// (lit) .. 0 (shadowed); a penumbra falls out of weighting nearer occluders more.
// Lts.z <= 0 (light below the tangent horizon) is left to the NdotL term.
float parallaxSelfShadow(vec2 uv, float h0, vec3 Lts) {
    if (Lts.z <= 0.0)
        return 1.0;
    const int LAYERS = 16;
    float dh = (1.0 - h0) / float(LAYERS);                       // march from the hit up to the crest
    vec2 duv = (Lts.xy / max(Lts.z, 0.1)) * parallaxScale * dh;  // floored like the view march
    float shadow = 0.0;
    vec2 curUV = uv;
    float rayH = h0;
    for (int i = 1; i <= LAYERS; i++) {
        rayH += dh;
        curUV += duv;
        float sH = texture(heightTex, curUV).r;
        if (sH > rayH)
            shadow = max(shadow, (sH - rayH) * (1.0 - float(i) / float(LAYERS)));
    }
    return 1.0 - clamp(shadow * 2.0, 0.0, 1.0);
}

// Color space conversions
vec3 sRGBToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 linearToSRGB(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Thin-film interference for iridescent coatings (pilot visor effect)
// thickness: film thickness in nanometers (200-600nm typical)
// cosTheta: dot(N, V) - viewing angle
// filmIOR: refractive index of the thin film coating (~1.5 for most coatings)
vec3 thinFilmInterference(float thickness, float cosTheta, float filmIOR) {
    // Wavelengths in nanometers for RGB
    const vec3 wavelengths = vec3(650.0, 550.0, 450.0); // R, G, B

    // Refracted angle in the film (Snell's law, assuming air n=1.0).
    // cosTheta comes from a normalized dot product that can round above 1;
    // an unclamped square makes these sqrt arguments negative, and the NaN
    // poisons the whole light sum into black view-dependent flecks
    cosTheta = clamp(cosTheta, 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float sinThetaFilm = sinTheta / filmIOR;
    float cosThetaFilm = sqrt(max(1.0 - sinThetaFilm * sinThetaFilm, 0.0));

    // Optical path difference (2 * n * d * cos(theta_film))
    float opd = 2.0 * filmIOR * thickness * cosThetaFilm;

    // Phase shift for each wavelength
    vec3 phase = 2.0 * PI * opd / wavelengths;

    // Interference: (1 + cos(phase)) / 2 gives 0-1 range
    // Add phase shift of PI for reflection from denser medium
    vec3 interference = 0.5 + 0.5 * cos(phase + PI);

    // Boost saturation for more vivid colors
    vec3 color = interference;
    float avg = (color.r + color.g + color.b) / 3.0;
    color = mix(vec3(avg), color, 1.5); // Increase saturation

    return clamp(color, 0.0, 1.0);
}

// GGX/Trowbridge-Reitz Normal Distribution Function
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// Charlie sheen distribution (Estevez-Kulla, "Production Friendly Microfacet
// Sheen"): an inverted-Gaussian NDF giving cloth its retroreflective grazing
// rim. sheenRoughness is used directly as alpha, per the glTF/KHR reference.
float distributionCharlie(vec3 N, vec3 H, float sheenRoughness) {
    float invAlpha = 1.0 / max(sheenRoughness, 0.0001);
    float NdotH = max(dot(N, H), 0.0);
    float sin2h = max(1.0 - NdotH * NdotH, 0.0078125); // fp16-safe floor
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * PI);
}

// Ashikhmin visibility term, paired with the Charlie NDF for sheen (glTF ref).
float visibilityAshikhmin(float NdotL, float NdotV) {
    return clamp(1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV)), 0.0, 1.0);
}

// Peak RGB channel (the engine's inline max(r, max(g, b)) idiom).
float maxComp(vec3 v) {
    return max(v.r, max(v.g, v.b));
}

// Resolved KHR sheen color: the factor, optionally modulated by the sheen color
// texture (unit 8, sRGB). (0,0,0) for a material carrying no sheen.
vec3 sheenColorAt(vec2 uv) {
    vec3 c = sheenColorFactor;
    if (sheenTexExists > 0) {
        c *= texture(sheenTex, uv).rgb;
    }
    return c;
}

// Anisotropic GGX distribution (for brushed metal, hair, etc.)
float distributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B, float roughness, float anisotropy) {
    float at = max(roughness * (1.0 + anisotropy), 0.001);
    float ab = max(roughness * (1.0 - anisotropy), 0.001);

    float ToH = dot(T, H);
    float BoH = dot(B, H);
    float NoH = max(dot(N, H), 0.0);

    float a2 = at * ab;
    vec3 v = vec3(ab * ToH, at * BoH, a2 * NoH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;

    return a2 * w2 * w2 / PI;
}

// Back-light transmission: approximate light scattering THROUGH a thin
// translucent surface toward the viewer (Barre-Brisebois). When a light sits
// behind the surface relative to the camera, a tinted (reddish) glow shows on
// the shadow side -- the backlit-wax/ear look. The screen-space SSS blur handles
// FRONT scatter; this is the separate transmitted term, added per light.
vec3 subsurfaceTransmission(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 tint, float strength,
                            vec3 lightColor) {
    const float distortion = 0.3; // bend the transmission direction by the normal
    const float power = 4.0;      // tighten the glow toward directly-behind lights
    vec3 tL = normalize(L + N * distortion);
    float t = pow(clamp(dot(V, -tL), 0.0, 1.0), power);
    return albedo * tint * lightColor * (t * strength);
}

// Smith's Schlick-GGX geometry function for a single direction
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

// Smith's geometry function combining view and light directions
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Attenuation for point/spot lights. The denominator floor guards degenerate
// all-zero coefficients (import sanitizes them, but bad data must not turn
// into +INF radiance -- one INF pixel survives every clamp downstream).
float calculateAttenuation(float distance, float constant, float linear, float quadratic) {
    return 1.0 / max(constant + linear * distance + quadratic * (distance * distance), 1e-4);
}

// 16-tap Poisson disk (unit radius) for the PCSS blocker search and filter.
// Sampled UNROTATED: a per-pixel rotation decorrelates the pattern between
// neighbours, which turns the 16-tap quantization into per-pixel shadow noise
// — invisible on diffuse but riding the sharp specular lobe into a field of
// bright speckle on the metal. An unrotated disk gives a spatially coherent,
// smooth penumbra instead; the modest radius cap keeps 16 taps free of banding.
const vec2 POISSON16[16] = vec2[](
    vec2(-0.9420, -0.3991), vec2(0.9456, -0.7689), vec2(-0.0942, -0.9294),
    vec2(0.3450, 0.2939), vec2(-0.9159, 0.4577), vec2(-0.8154, -0.8791),
    vec2(-0.3828, 0.2768), vec2(0.9748, 0.7565), vec2(0.4432, -0.9751),
    vec2(0.5374, -0.4737), vec2(-0.2650, -0.4189), vec2(0.7920, 0.1909),
    vec2(-0.2419, 0.9971), vec2(-0.8141, 0.9144), vec2(0.1998, 0.7864),
    vec2(0.1438, -0.1410));

// Max PCSS filter/search radius in shadow-UV. Raising this past ~10 texels
// (2048 map) reintroduces shadow noise that rides the specular into speckle
// with a 16-tap unrotated disk — a hard cleanliness cap, not a look knob.
const float PCSS_MAX_RADIUS_UV = 0.005;
// Penumbra growth per unit of blocker-receiver separation.
const float PCSS_PENUMBRA_SCALE = 4.0;

// Ortho shadow depth is linear in [near,far]; recover world-space distance
// from the light so blocker/receiver separation is a real length. nf is the
// sampled cascade's ortho near/far pair (cascadeParams.yz).
float linearizeOrthoDepth(float d01, vec2 nf) {
    return nf.x + d01 * (nf.y - nf.x);
}

// Fixed 3x3 PCF: the pre-PCSS path, kept bit-identical as the fallback.
// layer is a shadow-array layer (slot * cascadeCount + cascade).
float shadowPCF3x3(int layer, vec2 uv, float currentDepth, float bias) {
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            float pcfDepth = texture(shadowMaps, vec3(uv + offset, float(layer))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return 1.0 - (shadow / 9.0);
}

// Contact-hardening soft shadow (PCSS). lightSize is the scalar emitter size
// (world units of the emitter disk); larger = softer, faster-growing penumbra
// with blocker distance. The algorithm is isotropic, so callers collapse a
// rectangular emitter to a single dimension. cascade is the fragment's
// cascade, hoisted by the caller (caster-independent).
// One cascade's shadow estimate for a projected position already known to be
// in bounds: PCSS when enabled and the emitter resolves, else 3x3 PCF.
float cascadeShadowTap(int layer, vec3 projCoords, float NdotL, float lightSize) {
    // cascadeParams.w = (legacyRange/range) * (width/legacyWidth): first
    // re-expresses the app-tuned 0..1 bias in this cascade's depth scale
    // (the range factor is what keeps the comparison dimensionally sound --
    // don't "simplify" it away), then grows it with the real texel size.
    // Exactly 1.0 at cascadeCount 1.
    float bias = max(shadowBias * (1.0 - NdotL), shadowBias * 0.1) * cascadeParams[layer].w;
    float currentDepth = projCoords.z;

    if (pcssEnabled == 0) {
        return shadowPCF3x3(layer, projCoords.xy, currentDepth, bias);
    }

    // The sampled cascade's ortho geometry: frustum width and near/far pair.
    // Wider (coarser) cascades shrink the same emitter's UV footprint, so the
    // penumbra stays a consistent world-space width across a cascade seam.
    float frustumWidth = cascadeParams[layer].x;
    vec2 nearFar = cascadeParams[layer].yz;

    // The emitter as a fraction of the shadow map. Capped low so the 16 taps
    // stay dense enough to be free of banding, and so the penumbra saturates
    // cleanly rather than growing past what the tap budget can resolve; the
    // penumbra still grows with blocker distance for contact hardening.
    float lightSizeUV =
        clamp(pcssSoftness * lightSize / frustumWidth, 0.0, PCSS_MAX_RADIUS_UV);
    if (lightSizeUV < shadowTexelSize.x) {
        // Emitter smaller than a texel: no meaningful penumbra, stay crisp
        return shadowPCF3x3(layer, projCoords.xy, currentDepth, bias);
    }

    // 1. Blocker search: average depth of texels nearer the light than the
    //    receiver, over a disk the size of the emitter's shadow footprint
    float zReceiver = linearizeOrthoDepth(currentDepth, nearFar);
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = POISSON16[i] * lightSizeUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(layer))).r;
        if (d < currentDepth - bias) {
            blockerSum += linearizeOrthoDepth(d, nearFar);
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5) {
        return 1.0; // no blockers: fully lit
    }
    float zBlocker = blockerSum / blockerCount;

    // 2. Penumbra: grows with blocker-receiver separation (feet touching the
    //    floor stay sharp; the head 2m up casts a soft edge). Normalized by
    //    frustum depth so the product with lightSizeUV is a clean ratio.
    float penumbra = (zReceiver - zBlocker) / (nearFar.y - nearFar.x);
    float filterRadiusUV =
        clamp(lightSizeUV * penumbra * PCSS_PENUMBRA_SCALE, shadowTexelSize.x, PCSS_MAX_RADIUS_UV);

    // 3. Variable-width PCF over the same disk
    float shadow = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = POISSON16[i] * filterRadiusUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(layer))).r;
        shadow += currentDepth - bias > d ? 1.0 : 0.0;
    }
    return 1.0 - (shadow / 16.0);
}

float calculateShadow(int shadowIndex, int cascade, vec3 worldPos, float NdotL, float lightSize) {
    // Union the occlusion of the fragment's cascade and every wider one
    // (visibility = MIN across the walk). A tight near box can clip a
    // distant occluder out of its depth render -- partially or entirely --
    // while still covering the receiver, so it under-reports: a hard
    // straight cut (or lightening band) across the shadow at the box edge
    // that MOVES with the camera. A tight map's partial answer never vetoes
    // a wider map's full one; wider maps only ever ADD occlusion they saw.
    // At cascadeCount 1 this is exactly the classic bounds-check + tap.
    float visibility = 1.0;
    for (int c = cascade; c < cascadeCount; c++) {
        int layer = shadowIndex * cascadeCount + c;
        vec4 fragPosLightSpace = lightSpaceMatrix[layer] * vec4(worldPos, 1.0);
        vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
        projCoords = projCoords * 0.5 + 0.5;

        if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
            projCoords.y < 0.0 || projCoords.y > 1.0) {
            continue;
        }

        visibility = min(visibility, cascadeShadowTap(layer, projCoords, NdotL, lightSize));
        if (visibility <= 0.0)
            break; // fully occluded; wider maps cannot add more
    }
    return visibility;
}

// Perspective spot (flashlight) shadow: 1 = lit, 0 = occluded. A single tap with
// a slope-scaled bias (grazing floor pool stays acne-free); out of the frustum
// (behind the light or past its far plane / cone) stays lit via the white border.
float calculateSpotShadow(vec3 worldPos, float NdotL) {
    vec4 ls = spotShadowMatrix * vec4(worldPos, 1.0);
    if (ls.w <= 0.0)
        return 1.0;
    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
    if (pc.z > 1.0 || pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0)
        return 1.0;
    float bias = max(0.0015 * (1.0 - NdotL), 0.0004);
    return (pc.z - bias > texture(spotShadowMap, pc.xy).r) ? 0.0 : 1.0;
}

// Per-pixel screen-space motion vector in UV units: current vs previous
// un-jittered clip position. Shared by the velocity G-buffer and the debug view.
vec2 screenVelocity() {
    return (CurrClip.xy / CurrClip.w - PrevClip.xy / PrevClip.w) * 0.5;
}

// Clearcoat normal: the geometric normal, perturbed by the coat normal map if
// present (glTF: the coat normal is independent of the base normal map). Only
// called from the coat lobes (clearcoat > 0), so a coat-free draw compiles the
// base path unchanged.
vec3 clearcoatNormal(vec2 uv) {
    vec3 Ngeo = normalize(Normal);
    if (!gl_FrontFacing) {
        Ngeo = -Ngeo;
    }
    if (clearcoatNormalExists > 0) {
        vec3 Tc = normalize(TBN[0] - dot(TBN[0], Ngeo) * Ngeo);
        // TangentW is the exact handedness bit (see the base normal path). On
        // back faces Ngeo is already flipped above, so the derived Bc flips
        // with it -- the basis stays consistent with the normal it shades,
        // where the old sign-recovery kept the front-face bitangent.
        vec3 Bc = cross(Ngeo, Tc) * TangentW;
        vec3 cn = texture(clearcoatNormalTex, uv).xyz * 2.0 - 1.0;
        return normalize(mat3(Tc, Bc, Ngeo) * cn);
    }
    return Ngeo;
}

// Weighted-blended OIT depth weight (McGuire & Bavoil 2013, eq. 9): nearer
// fragments dominate the accumulation, so the composite approximates back-to-
// front order without sorting. z is positive view distance (-ViewPos.z).
float oitWeight(float z) {
    return clamp(0.03 / (1e-5 + pow(z / 200.0, 4.0)), 1e-2, 3e3);
}

void main() {
    // Early-out for simple render modes that don't need texture sampling
    if (renderMode == 5) {
        // Flat Color - no textures needed
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
        return;
    }
    if (renderMode == 9) {
        // Motion-vector visualization: per-pixel screen velocity as color
        // (mid-gray = static; the large scale just makes small per-frame motion
        // visible — X motion tints red, Y motion tints green). Static geometry
        // resolves to exactly 0.5 gray regardless of scale, so there is no false
        // colour when still.
        vec2 velUv = screenVelocity();
        FragColor = vec4(clamp(0.5 + velUv * 400.0, 0.0, 1.0), 0.5, 1.0);
        return;
    }
    if (renderMode == 1) {
        // Normals Visualization - no textures needed
        vec3 color = normalize(Normal) * 0.5 + 0.5;
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 2) {
        // World Position Visualization - no textures needed
        vec3 color = fract(WorldPos * 0.01);
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 3) {
        // Texture Coordinates Visualization - no textures needed
        FragColor = vec4(TexCoords, 0.0, 1.0);
        return;
    }
    if (renderMode == 4) {
        // Tangent Space Visualization - no textures needed
        vec3 tangent = normalize(TBN[0]) * 0.5 + 0.5;
        FragColor = vec4(tangent, 1.0);
        return;
    }
    if (renderMode == 6) {
        // Albedo Only - only sample albedo texture
        vec2 uvAlbedo = transformUV(TexCoords);
        vec3 albedoMapOnly = albedo;
        float texAlphaOnly = 1.0;
        if (albedoTexExists > 0) {
            // sRGB texture: the hardware already decoded the sample to linear
            vec4 albedoSample = texture(albedoTex, uvAlbedo);
            albedoMapOnly = albedo * albedoSample.rgb;
            texAlphaOnly = albedoSample.a;
        }
        // Apply vertex color
        if (vertexColorExists > 0) {
            albedoMapOnly *= sRGBToLinear(VertexColor.rgb);
            texAlphaOnly *= VertexColor.a;
        }
        // Alpha cutoff for hair/foliage
        if (alphaBelowCutoff(texAlphaOnly)) {
            discard;
        }
        vec3 color = linearToSRGB(albedoMapOnly);
        FragColor = vec4(color, materialOpacity * texAlphaOnly);
        return;
    }

    // Apply UV transform for KHR_texture_transform
    vec2 uv = transformUV(TexCoords);

    // Parallax occlusion mapping (§4.11): offset the UV by marching the height
    // field in tangent space, BEFORE every material sampler below (all of them
    // read `uv`). Guarded so a material without POM (no height map / scale 0 /
    // --no-parallax) runs the exact pre-feature path. TBN maps tangent->world, so
    // its transpose takes the world-space view direction into tangent space.
    // POM active for this material (§4.11). Named once and reused by the
    // self-shadow in the light loop; it is a bool of integer/uniform comparisons,
    // so the OFF path stays byte-identical to the pre-feature code.
    bool pom = parallaxEnabled > 0 && heightTexExists > 0 && parallaxScale > 0.0;
    float parallaxHeight = 0.0; // height at the POM hit, for the self-shadow march
    if (pom) {
        vec3 Vts = normalize(transpose(TBN) * normalize(camPos - WorldPos));
        vec2 uv0 = uv;
        uv = parallaxOcclusion(uv, Vts);
        // Silhouette clipping: when the march pushed the UV past the [0,1] relief
        // tile, that surface has receded beyond the mesh edge -- drop the fragment
        // so the background shows through instead of a flat cutoff, giving the
        // contour a relief-shaped edge at grazing. Gated on uv0 being inside the
        // first tile so tiled/wrapped UVs (whose relief is interior, not at a mesh
        // edge) are never clipped mid-surface. Assumes the relief maps 1:1 to the
        // surface -- the standard silhouette-POM constraint.
        if (uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0 &&
            (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0))
            discard;
        parallaxHeight = texture(heightTex, uv).r;
    }

    // Sample material properties. glTF semantics: the scalar factors
    // modulate the texture (effective value = factor * texture), so a
    // material can globally tint or gloss up its maps
    vec3 albedoMap = albedo;
    float texAlpha = 1.0;  // Alpha from albedo texture (for hair/foliage)
    if (albedoTexExists > 0) {
        // sRGB texture: the hardware already decoded the sample to linear
        vec4 albedoSample = texture(albedoTex, uv);
        albedoMap = albedo * albedoSample.rgb;
        texAlpha = albedoSample.a;
    }

    // Apply vertex color to tint albedo (glTF vertex colors)
    if (vertexColorExists > 0) {
        albedoMap *= sRGBToLinear(VertexColor.rgb);
        texAlpha *= VertexColor.a;
    }

    // Alpha cutoff for hair/foliage - discard early before expensive lighting
    if (alphaBelowCutoff(texAlpha)) {
        discard;
    }

    vec3 N;
    if (normalTexExists > 0) {
        // Re-orthonormalize the interpolated TBN (Gram-Schmidt) before
        // applying the normal map. Vertex interpolation — and especially
        // skinning, where cross-rig retargeting puts non-uniform scale/shear
        // in the bone matrices — leaves the raw TBN non-orthonormal, which
        // skews the normal-mapped normal (worst where the normal map is
        // steep) into bright specks under the key lights on deformed poses.
        // The geometric normal itself stays clean, so this never showed in
        // the Normals render view (which visualizes the geometric normal).
        vec3 Ng = normalize(Normal);
        vec3 T = normalize(TBN[0] - dot(TBN[0], Ng) * Ng);
        // The handedness comes over as a flat varying -- the exact bit, not a
        // sign recovered by dotting against the interpolated TBN[1], which
        // goes near-degenerate on slivers and mirrored-UV seams.
        vec3 B = cross(Ng, T) * TangentW;
        vec3 nTex = texture(normalTex, uv).rgb * 2.0 - 1.0;
        // Apply normal scale to XY components (glTF normalTexture.scale)
        nTex.xy *= normalScale;
        N = normalize(mat3(T, B, Ng) * nTex);
    } else {
        N = normalize(Normal);
    }
    // Double-sided surfaces (hair cards, foliage): the back face must be lit
    // as its own side, or it faces away from every light and renders black
    if (!gl_FrontFacing) {
        N = -N;
    }

    float roughnessMap = roughness;
    if (roughnessLayer >= 0) {
        // glTF: G channel contains roughness (works for grayscale too since R=G=B)
        roughnessMap = roughness * texture(maskArray, vec3(uv, float(roughnessLayer))).g;
    }
    // Clamp roughness to avoid division issues
    roughnessMap = clamp(roughnessMap, 0.04, 1.0);

    float metallicMap = metallic;
    if (metallicLayer >= 0) {
        // glTF: B channel contains metallic (works for grayscale too since R=G=B)
        metallicMap = metallic * texture(maskArray, vec3(uv, float(metallicLayer))).b;
    }

    float aoMap = ao;
    if (aoLayer >= 0) {
        // Use UV1 for AO if available (common glTF lightmap pattern), otherwise
        // UV0 -- but only when UV1 actually holds texture coordinates. Under
        // the vegetation wind modes it holds (branch phase, flex weight), and
        // sampling a map with those would produce plausible-looking garbage
        // with nothing to warn about, so those materials fall back to UV0.
        vec2 aoUV = (texCoords2Exists > 0 && uWindMode == 0) ? TexCoords2 : uv;
        // Apply occlusion strength (glTF occlusionTexture.strength)
        float sampledAo = texture(maskArray, vec3(aoUV, float(aoLayer))).r;
        aoMap = mix(1.0, sampledAo, aoStrength);
    }

    vec3 emissiveMap = vec3(0.0);
    if (emissiveTexExists > 0) {
        // sRGB texture: the hardware already decoded the sample to linear
        vec3 texEmissive = texture(emissiveTex, uv).rgb;
        // Scale by emissiveFactor if set, otherwise use texture directly (backward compat)
        float factorSum = emissiveFactor.r + emissiveFactor.g + emissiveFactor.b;
        emissiveMap = texEmissive * (factorSum > 0.001 ? emissiveFactor : vec3(1.0));
    } else {
        emissiveMap = emissiveFactor;
    }

    float opacity = materialOpacity;
    if (opacityLayer >= 0) {
        opacity = texture(maskArray, vec3(uv, float(opacityLayer))).r * materialOpacity;
    } else if (texAlpha < 1.0) {
        // Use albedo texture alpha if no separate opacity texture
        opacity = texAlpha * materialOpacity;
    }

    // Microsurface detail - modulates roughness for fine surface detail
    if (microsurfaceLayer >= 0) {
        float detail = texture(maskArray, vec3(uv, float(microsurfaceLayer))).r;
        roughnessMap = clamp(roughnessMap * (0.5 + detail), 0.04, 1.0);
    }

    // Anisotropy - for brushed metal, hair effects
    float anisotropyMap = 0.0;
    if (anisotropyLayer >= 0) {
        anisotropyMap = texture(maskArray, vec3(uv, float(anisotropyLayer))).r;
    }


    // Geometric specular anti-aliasing (Kaplanyan 2016): where the normal
    // varies quickly within a pixel (fine normal-mapped detail under
    // minification), the true reflectance is a wider lobe than any single
    // normal's — widen roughness by the normal variance to match, killing
    // the sparkle/shimmer that GGX otherwise produces in motion
    if (specularAAStrength > 0.0) {
        vec3 dndu = dFdx(N);
        vec3 dndv = dFdy(N);
        float variance =
            specularAAStrength * specularAAStrength * 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
        float kernelRoughness2 = min(2.0 * variance, 0.18);
        float a2 = roughnessMap * roughnessMap;
        roughnessMap = clamp(sqrt(a2 + kernelRoughness2), roughnessMap, 1.0);
    }

    // Calculate view direction (WorldPos -- world space, as the maths needs)
    vec3 V = normalize(camPos - WorldPos);

    // Render modes that need texture data
    if (renderMode == 7) {
        // Simple Diffuse Lighting (same fused dir+cluster fetch as the PBR loop)
        vec3 Lo = vec3(0.0);
        int sTileX = min(int(gl_FragCoord.x * clusterParams.z), CLUSTER_X - 1);
        int sTileY = min(int(gl_FragCoord.y * clusterParams.w), CLUSTER_Y - 1);
        int sSlice = clamp(int(log2(max(-ViewPos.z, 1e-4)) * clusterParams.x + clusterParams.y),
                           0, CLUSTER_Z - 1);
        uint sWord = clusterWord(uint(sTileX + CLUSTER_X * (sTileY + CLUSTER_Y * sSlice)));
        uint sOffset = sWord >> 12u;
        int sCount = int(sWord & 0xFFFu);
        int sNumDir = lightCounts.x;
        for (int k = 0; k < sNumDir + sCount; k++) {
            vec3 L;
            float attenuation;
            vec3 lightCI;
            if (k < sNumDir) {
                L = normalize(-dirLights[k].dirShadow.xyz);
                attenuation = 1.0;
                lightCI = dirLights[k].colorIntensity.xyz;
            } else {
                uint li = lightIndexAt(sOffset + uint(k - sNumDir));
                vec3 lightPos = clusterLights[li].posRange.xyz;
                L = normalize(lightPos - WorldPos);
                float distance = length(lightPos - WorldPos);
                attenuation = calculateAttenuation(distance, clusterLights[li].attenCutoff.x,
                                                   clusterLights[li].attenCutoff.y,
                                                   clusterLights[li].attenCutoff.z);
                attenuation *=
                    spotConeFactorP(clusterLights[li].dirType.w, clusterLights[li].dirType.xyz,
                                    clusterLights[li].attenCutoff.w,
                                    clusterLights[li].spotShadowSize.x, L);
                lightCI = clusterLights[li].colorIntensity.xyz;
            }
            float NdotL = max(dot(N, L), 0.0);
            Lo += albedoMap * lightCI * attenuation * NdotL;
        }
        vec3 color = Lo + vec3(0.03) * albedoMap;
        color = color / (color + vec3(1.0));
        color = linearToSRGB(color);
        FragColor = vec4(color, opacity);
        return;
    }
    if (renderMode == 8) {
        // Metallic and Roughness Visualization
        FragColor = vec4(metallicMap, roughnessMap, 0.0, 1.0);
        return;
    }

    // renderMode == 0: Full PBR

    // Calculate F0 (surface reflection at zero incidence) from IOR
    // F0 = ((ior - 1) / (ior + 1))^2
    // For plastic/glass (ior=1.5): F0 = 0.04
    // Squared by multiplication: pow is undefined for the negative base an
    // ior below 1 would produce
    float iorF0Base = (ior - 1.0) / (ior + 1.0);
    float iorF0 = iorF0Base * iorF0Base;
    vec3 F0 = vec3(iorF0);
    // KHR_materials_specular re-parameterizes the DIELECTRIC F0: specularColorFactor
    // tints it and specularFactor weights it (metals keep their albedo-derived F0
    // through the mix below, so both are dielectric-only). Folding the weight into
    // F0 -- rather than scaling the specular term -- keeps analytic and IBL specular
    // consistent AND byte-identical off (grazing Fresnel still reaches 1, so the
    // weight approximates KHR's full-angle dim; a documented v1 simplification).
    // Guarded on the -1 sentinel so non-specular materials compile the exact
    // original F0 = vec3(iorF0) expression -> byte-identical.
    if (specularEnabled > 0 && specularFactor >= 0.0) {
        F0 = min(F0 * specularColorFactor * specularFactor, vec3(1.0));
    }
    F0 = mix(F0, albedoMap, metallicMap);

    float NdotV = max(dot(N, V), 0.0);

    // Transmission is only real when this pass has the opaque resolve to
    // sample; without it (refraction disabled, debug modes, probe capture)
    // it must read as 0 so the diffuse term it would replace stays lit --
    // glass falls back to a plain lit surface instead of going dark
    float transmissionEff = sceneColorAvailable > 0 ? transmission : 0.0;

    // Multi-scatter energy compensation (Kulla-Conty / Fdez-Agüera):
    // single-scatter GGX discards light that bounces between microfacets
    // more than once, dimming rough metals. The split-sum LUT's A+B is the
    // single-scatter directional albedo Ess; scaling specular by
    // 1 + F0*(1/Ess - 1) restores the multi-bounce energy (Fresnel-
    // weighted: dielectrics barely move, metals recover fully).
    vec2 brdf = vec2(0.0);
    vec3 energyComp = vec3(1.0);
    if (iblEnabled > 0) {
        // Fetched inside the gate: with no environment the LUT is unbound
        // and must not be sampled. The ambient block below reuses this
        // fetch (same coordinates).
        brdf = texture(brdfLUT, vec2(NdotV, roughnessMap)).rg;
        float Ess = brdf.x + brdf.y;
        if (energyCompEnabled > 0 && Ess > 1e-4) {
            energyComp = 1.0 + F0 * (1.0 / Ess - 1.0);
        }
    }

    // Analytic keys act as small area lights (sphere-light approximation,
    // Karis 2013): widen the GGX lobe by the light's angular size and
    // scale by (a/a')^2 so reflected energy is conserved. Point lights
    // otherwise produce needle lobes on polished texels that alias into
    // per-pixel white speckle at close range — and widening WITHOUT the
    // renormalization saturates entire surfaces instead. IBL is untouched
    // and keeps the crisp environment reflections.
    float ggxAlpha = roughnessMap * roughnessMap;
    float areaAlpha = min(ggxAlpha + 0.05, 1.0);
    float areaLightNorm = (ggxAlpha / areaAlpha) * (ggxAlpha / areaAlpha);
    float areaLightRoughness = sqrt(areaAlpha);

    // Accumulate lighting from all lights
    vec3 Lo = vec3(0.0);

    // SSS: separately accumulate skin diffuse irradiance into DiffuseOut
    // (blurred + recomposited by the SSS post pass). Guarded so a non-skin
    // material or --no-sss leaves the FragColor path byte-identical; the taps
    // below REUSE the exact diffuse sub-expression already in Lo/ambient and
    // never touch those accumulations.
    bool sss = sssEnabled > 0 && subsurface > 0.0;
    vec3 sssDiffuse = vec3(0.0);

    // Get tangent and bitangent for anisotropy
    vec3 T = normalize(TBN[0]);
    vec3 B = normalize(TBN[1]);

    // The fragment's shadow cascade is caster-independent: hoist it out of
    // the light loop (mirrors catcher_frag)
    int fragCascade = selectCascade(-ViewPos.z);

    // Clustered forward (spec 9.1): directional lights shade unconditionally
    // (they reach every fragment), then the fragment's cluster supplies the
    // point/spot/area list. Radiance uses the CPU-premultiplied
    // color*intensity, and the directional CSM slot rides in the DirLight
    // itself -- each light samples its OWN shadow slot, immune to the light
    // ordering (the retired per-node path matched shadow slots by loop index,
    // which shimmered when the k-nearest heap reordered directionals).
    int tileX = min(int(gl_FragCoord.x * clusterParams.z), CLUSTER_X - 1);
    int tileY = min(int(gl_FragCoord.y * clusterParams.w), CLUSTER_Y - 1);
    int slice = clamp(int(log2(max(-ViewPos.z, 1e-4)) * clusterParams.x + clusterParams.y), 0,
                      CLUSTER_Z - 1);
    uint clusterData = clusterWord(uint(tileX + CLUSTER_X * (tileY + CLUSTER_Y * slice)));
    uint clusterOffset = clusterData >> 12u;
    int clusterCount = int(clusterData & 0xFFFu);
    int numDir = lightCounts.x;

    for (int k = 0; k < numDir + clusterCount; k++) {
        vec3 L;
        float attenuation;
        vec3 lightCI;   // color * intensity (premultiplied on CPU)
        vec2 lightSize; // PCSS emitter size
        int dirShadowSlot = -1;
        bool isSpot = false;

        if (k < numDir) {
            L = normalize(-dirLights[k].dirShadow.xyz);
            attenuation = 1.0;
            lightCI = dirLights[k].colorIntensity.xyz;
            lightSize = dirLights[k].sizeMisc.xy;
            dirShadowSlot = int(dirLights[k].dirShadow.w);
        } else {
            uint li = lightIndexAt(clusterOffset + uint(k - numDir));
            vec3 lightPos = clusterLights[li].posRange.xyz;
            L = normalize(lightPos - WorldPos);
            float distance = length(lightPos - WorldPos);
            attenuation = calculateAttenuation(distance, clusterLights[li].attenCutoff.x,
                                               clusterLights[li].attenCutoff.y,
                                               clusterLights[li].attenCutoff.z);
            attenuation *=
                spotConeFactorP(clusterLights[li].dirType.w, clusterLights[li].dirType.xyz,
                                clusterLights[li].attenCutoff.w,
                                clusterLights[li].spotShadowSize.x, L);
            lightCI = clusterLights[li].colorIntensity.xyz;
            lightSize = clusterLights[li].spotShadowSize.zw;
            isSpot = clusterLights[li].dirType.w == 2.0;
        }

        // Half vector, guarded: where the light is directly behind the
        // fragment along the view ray, V + L vanishes and normalize()
        // returns garbage or NaN — and NaN * NdotL(0) stays NaN, painting
        // black flecks around the light's vanishing point. The fallback N
        // is harmless: NdotL is ~0 in that configuration anyway.
        vec3 Hraw = V + L;
        float hLen2 = dot(Hraw, Hraw);
        vec3 H = hLen2 > 1e-8 ? Hraw * inversesqrt(hLen2) : N;
        vec3 radiance = lightCI * attenuation;

        // Cook-Torrance BRDF with optional anisotropy
        float NDF;
        if (anisotropyLayer >= 0 && anisotropyMap > 0.01) {
            NDF = distributionGGXAnisotropic(N, H, T, B, roughnessMap, anisotropyMap);
        } else {
            NDF = areaLightNorm * distributionGGX(N, H, areaLightRoughness);
        }
        float G = geometrySmith(N, V, L, roughnessMap);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        // Apply thin-film interference for iridescent coatings (pilot visor style)
        if (filmThickness > 0.0) {
            vec3 iridescence = thinFilmInterference(filmThickness, NdotV, 1.5);
            // Strong iridescent mirror effect
            // Clamped: pow with a negative base (NdotV rounding above 1) is
            // undefined in GLSL and yields NaN on this driver
            float fresnel = pow(clamp(1.0 - NdotV, 0.0, 1.0), 2.0);
            // Replace F entirely with strong iridescent reflection
            F = iridescence * (0.6 + fresnel * 0.4);
        }

        float NdotL = max(dot(N, L), 0.0);

        // Specular contribution, multi-scatter compensated
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * NdotV * NdotL + 0.0001;
        vec3 specular = numerator / denominator * energyComp;

        // Energy conservation: diffuse and specular must not exceed 1.0
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        // Metals have no diffuse reflection; transmission replaces diffuse
        // with the transmitted scene term added after the loop (KHR:
        // mix(diffuse_brdf, specular_btdf, transmission) under specular)
        kD *= 1.0 - metallicMap;
        kD *= 1.0 - transmissionEff;

        // Directional shadow: the light's own CSM slot. Alpha-to-coverage
        // surfaces (hair cards) cast shadows but never receive the shadow
        // map: at map-texel scale (millimeters) card-on-card comparisons
        // are pure acne, drawing card-shaped streaks through the hair.
        // Their self-occlusion comes from the AO texture and SSAO instead.
        float shadow = 1.0;
        if (dirShadowSlot >= 0 && (alphaMasked == 0 || foliageShadows == 1)) {
            shadow = calculateShadow(dirShadowSlot, fragCascade, WorldPos, NdotL,
                                     max(lightSize.x, lightSize.y));
        }
        // Spot (flashlight) shadow: its own perspective map
        if (isSpot && spotShadowActive == 1 && alphaMasked == 0) {
            shadow = calculateSpotShadow(WorldPos, NdotL);
        }

        // POM self-shadow
        if (pom) {
            shadow *= parallaxSelfShadow(uv, parallaxHeight, normalize(transpose(TBN) * L));
        }

        // Clearcoat: a second thin smooth dielectric GGX lobe (fixed F0 = 0.04,
        // IOR 1.5) over the base, on the coat normal Nc. The base term is
        // attenuated by the coat's Fresnel -- light the coat reflects never
        // reaches it (Kelemen-Szirmay-Kalos / Filament layering).
        vec3 coatSpec = vec3(0.0);
        float coatAtten = 0.0;
        if (clearcoatEnabled > 0 && clearcoat > 0.0) {
            vec3 Nc = clearcoatNormal(uv);
            float ccR = clamp(clearcoatRoughness, 0.04, 1.0);
            float Dc = distributionGGX(Nc, H, ccR);
            float Gc = geometrySmith(Nc, V, L, ccR);
            float Fc = fresnelSchlick(max(dot(H, V), 0.0), vec3(0.04)).r;
            coatSpec = vec3(clearcoat * Dc * Gc * Fc / denominator);
            coatAtten = clearcoat * Fc;
        }

        // Sheen (KHR_materials_sheen): a retroreflective Charlie lobe for cloth
        // (velvet / satin) -- no Fresnel, per glTF. Added over the base, which is
        // scaled by (1 - max(sheenColor)) as a cheap directional-albedo
        // approximation (a baked Charlie E-LUT is the follow-up).
        vec3 sheenSpec = vec3(0.0);
        float sheenScale = 1.0;
        if (sheenEnabled > 0 && maxComp(sheenColorFactor) > 0.0) {
            vec3 sheenColor = sheenColorAt(uv);
            float shR = clamp(sheenRoughnessFactor, 0.07, 1.0);
            float Dsh = distributionCharlie(N, H, shR);
            float Vsh = visibilityAshikhmin(NdotL, NdotV);
            sheenSpec = sheenColor * Dsh * Vsh;
            sheenScale = 1.0 - maxComp(sheenColor);
        }

        // Add this light's contribution with shadow. Firefly clamp: a
        // sub-pixel GGX spike carries far more energy than the pixel
        // legitimately integrates, aliasing into white sparkle across
        // low-roughness normal-mapped surfaces (and before the fp16 clamp,
        // overflowing to NaN). Highlights saturate the tonemap well below
        // this cap, so only the aliasing energy is lost.
        Lo += min((((kD * albedoMap / PI + specular) * sheenScale + sheenSpec) * (1.0 - coatAtten) +
                   coatSpec) *
                      radiance * NdotL * shadow,
                  vec3(10.0));

        // SSS: tap this light's Lambert diffuse into the separated skin-diffuse
        // buffer (blurred later; specular stays in FragColor). Separate accumulate
        // -- does not modify the Lo expression above.
        if (sss) {
            sssDiffuse += kD * albedoMap / PI * radiance * NdotL * shadow;
        }

        // SSS back-light transmission: thin-region glow when the light is
        // behind the surface. Guarded so non-skin / --no-sss is byte-identical.
        if (sss) {
            Lo += subsurfaceTransmission(N, L, V, albedoMap, subsurfaceColor, subsurface,
                                         lightCI * attenuation);
        }
    }

    // Ambient lighting with IBL
    vec3 ambient;
    if (iblEnabled > 0) {
        vec3 F = fresnelSchlickRoughness(NdotV, F0, roughnessMap);

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallicMap;
        kD *= 1.0 - transmissionEff; // diffuse yields to the transmitted term

        // Diffuse IBL: sample irradiance map with surface normal
        vec3 irradiance = texture(irradianceMap, N).rgb;
        vec3 diffuse = irradiance * albedoMap;

        // Specular IBL: sample prefiltered env map with reflection vector
        vec3 R = reflect(-V, N);
        vec3 prefilteredColor;
        if (probeEnabled > 0) {
            // Parallax correction: intersect the world reflection ray with
            // the probe's proxy AABB and sample toward the intersection. The
            // correction degenerates at the proxy walls (it collapses toward
            // the surface point), so feather it back to the plain reflection
            // direction near the box faces; a ray starting outside the box
            // (t <= 0) keeps the uncorrected direction too.
            vec3 invR = 1.0 / R;
            vec3 tMax3 = max((probeBoxMax - WorldPos) * invR, (probeBoxMin - WorldPos) * invR);
            float t = min(min(tMax3.x, tMax3.y), tMax3.z);
            vec3 corrected = (t > 0.0) ? normalize((WorldPos + R * t) - probePos) : R;
            vec3 boxCenter = 0.5 * (probeBoxMin + probeBoxMax);
            vec3 halfExt = max(0.5 * (probeBoxMax - probeBoxMin), vec3(1e-4));
            vec3 dd = abs(WorldPos - boxCenter) / halfExt;
            float inside = 1.0 - smoothstep(1.0 - probeBoxFade, 1.0,
                                            max(dd.x, max(dd.y, dd.z)));
            vec3 dir = normalize(mix(R, corrected, inside));
            prefilteredColor = textureLod(prefilteredMap, dir, roughnessMap * probeMaxLOD).rgb
                               * probeIntensity;
        } else {
            prefilteredColor = textureLod(prefilteredMap, R, roughnessMap * maxReflectionLOD).rgb;
        }
        // Reuses the brdf fetched before the light loop (same coordinates). The
        // KHR_materials_specular tint + weight reach IBL specular through F (F0 was
        // re-parameterized above), so no change to this line is needed.
        vec3 specular = prefilteredColor * (F * brdf.x + brdf.y) * energyComp;

        ambient = (kD * diffuse + specular) * aoMap * iblIntensity;

        // SSS: tap the IBL Lambert diffuse into the skin-diffuse buffer too
        // (reuses the exact sub-expression; does not touch the ambient line).
        if (sss) {
            sssDiffuse += kD * diffuse * aoMap * iblIntensity;
        }

        // Clearcoat IBL: a second env reflection at the coat roughness (its own
        // split-sum with F0 = 0.04) around the coat normal Nc, attenuating the
        // base ambient by the coat's grazing Fresnel. Reuses the prefiltered env
        // + BRDF LUT -- no new sampler. Plain reflection (no parallax proxy).
        if (clearcoatEnabled > 0 && clearcoat > 0.0) {
            vec3 Nc = clearcoatNormal(uv);
            float ccR = clamp(clearcoatRoughness, 0.04, 1.0);
            float NcdotVi = max(dot(Nc, V), 0.0);
            vec3 Rc = reflect(-V, Nc);
            float ccF = fresnelSchlickRoughness(NcdotVi, vec3(0.04), ccR).r * clearcoat;
            vec2 ccBrdf = texture(brdfLUT, vec2(NcdotVi, ccR)).rg;
            vec3 ccPre = textureLod(prefilteredMap, Rc, ccR * maxReflectionLOD).rgb;
            vec3 coatIBL = clearcoat * ccPre * (0.04 * ccBrdf.x + ccBrdf.y);
            ambient = ambient * (1.0 - ccF) + coatIBL * aoMap * iblIntensity;
        }

        // Sheen IBL: the Charlie cloth lobe lit by the environment. Reuses the
        // prefiltered env at the sheen roughness (no new sampler). With no baked
        // Charlie directional-albedo (E) LUT, the env sheen is concentrated toward
        // grazing by pow(1 - NdotV, 2) -- an approximation of the retroreflective
        // rim that avoids flooding the whole surface white (the E-LUT is the
        // follow-up). Guarded so the base ambient lines above stay byte-identical
        // when sheen is off.
        if (sheenEnabled > 0 && maxComp(sheenColorFactor) > 0.0) {
            vec3 sheenColor = sheenColorAt(uv);
            float shR = clamp(sheenRoughnessFactor, 0.07, 1.0);
            vec3 sheenPre = textureLod(prefilteredMap, R, shR * maxReflectionLOD).rgb;
            float sheenE = pow(1.0 - NdotV, 2.0);
            // Dim the base ambient like the analytic path (and the coat IBL above),
            // then add the grazing-concentrated sheen -- keeps the layer-over-base
            // energy convention consistent across analytic and IBL.
            ambient = ambient * (1.0 - maxComp(sheenColor)) +
                      sheenColor * sheenPre * sheenE * aoMap * iblIntensity;
        }
    } else {
        // Fallback to simple ambient when IBL is disabled (diffuse-only, so
        // it yields to transmission like the other diffuse terms)
        ambient = vec3(0.03) * albedoMap * aoMap * (1.0 - transmissionEff);
    }

    // Screen-space transmission (KHR_materials_transmission): the diffuse
    // term the kD scaling gave up comes back as light seen THROUGH the
    // surface -- the resolved opaque scene color, tinted by base color,
    // roughness-blurred via the resolve's mip chain, and bent by the
    // volume thickness along the refracted view ray. Thickness 0 (thin
    // glass) projects to exactly the fragment's own screen position: tint
    // and blur without a bend, per the glTF spec. Specular and emissive
    // are untouched -- glass keeps its reflections.
    vec3 transmitted = vec3(0.0);
    if (transmissionEff > 0.0) {
        vec3 refrDir = refract(-V, N, 1.0 / ior);
        vec3 exitPos = WorldPos + refrDir * transmissionThickness;
        // Right-associated: two mat4*vec4 instead of a per-fragment
        // mat4*mat4 (compilers do not reliably hoist the uniform product)
        vec4 refrClip = projection * (view * vec4(exitPos, 1.0));
        vec2 refrUV = clamp(refrClip.xy / refrClip.w * 0.5 + 0.5, 0.0, 1.0);
        // Box mips: one level per doubling of blur width; frosted surfaces
        // read a progressively softer background
        vec3 sceneSample =
            textureLod(sceneColorTex, refrUV, roughnessMap * TRANSMISSION_MAX_LOD).rgb;
        vec3 Ft = fresnelSchlickRoughness(NdotV, F0, roughnessMap);
        transmitted =
            sceneSample * albedoMap * (1.0 - Ft) * transmissionEff * (1.0 - metallicMap);
    }

    // Final color, linear HDR: tone mapping and gamma happen in the post
    // pass (tonemap_frag.glsl) after MSAA resolve and bloom.
    // Clamped below half-float max (65504): the scene framebuffer is
    // RGBA16F, and a tight GGX spike under the key lights (peak ~1.2e5 at
    // the 0.04 roughness floor, times grazing Fresnel) overflows the store
    // to +INF, which the tonemap turns into NaN and displays as black
    // flecks tracing the specular highlights.
    vec3 color = min(ambient + Lo + transmitted + emissiveMap, vec3(60000.0));

    // Cascade acceptance view: tint by the fragment's selected cascade so
    // split geometry and snap stability are visible (dead when csmDebug 0)
    if (csmDebug > 0 && cascadeCount > 1) {
        vec3 tint = fragCascade == 0   ? vec3(1.0, 0.35, 0.35)
                    : fragCascade == 1 ? vec3(0.35, 1.0, 0.35)
                                       : vec3(0.35, 0.55, 1.0);
        color = mix(color, tint, 0.35);
    }

    // Cluster occupancy heatmap: tint by this fragment's cluster light count
    // (blue 1 .. red >= 16; empty clusters stay untinted). Dead when
    // clusterDebug 0.
    if (clusterDebug > 0) {
        int dTileX = min(int(gl_FragCoord.x * clusterParams.z), CLUSTER_X - 1);
        int dTileY = min(int(gl_FragCoord.y * clusterParams.w), CLUSTER_Y - 1);
        int dSlice = clamp(int(log2(max(-ViewPos.z, 1e-4)) * clusterParams.x + clusterParams.y),
                           0, CLUSTER_Z - 1);
        uint dWord = clusterWord(uint(dTileX + CLUSTER_X * (dTileY + CLUSTER_Y * dSlice)));
        int dCount = int(dWord & 0xFFFu);
        if (dCount > 0) {
            float t = clamp(float(dCount) / 16.0, 0.0, 1.0);
            vec3 ramp = t < 0.25   ? mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), t * 4.0)
                        : t < 0.5  ? mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), (t - 0.25) * 4.0)
                        : t < 0.75 ? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), (t - 0.5) * 4.0)
                                   : mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (t - 0.75) * 4.0);
            color = mix(color, ramp, 0.6);
        }
    }

    // For translucent materials, apply Fresnel-based alpha
    // Edges become more reflective (less transparent) at glancing angles
    float finalOpacity = opacity;
    if (opacity < 1.0) {
        float fresnelOpacity = iorF0 + (1.0 - iorF0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
        // Blend between base opacity and full opacity based on Fresnel
        finalOpacity = mix(opacity, 1.0, fresnelOpacity);
    }

    FragColor = vec4(color, finalOpacity);

    // G-buffer: view-space normal (xyz) for SSAO; alpha is a non-negative
    // reflective marker — only the shadow catcher's negative alpha traces in
    // SSR, so model surfaces stamp 0 (never reflected in screen space; they
    // rely on IBL). Alpha-to-coverage surfaces (hair) stamp zero normals
    // instead: consumers must not trust normals at strand scale, and leaving
    // the buffer unwritten is worse (stale normals of whatever drew behind
    // the hair, under the hair's depth). Their alpha must mirror FragColor's:
    // Apple's driver derives A2C coverage from the last color output's alpha,
    // not output 0, and any other value reshapes the card cutouts.
    NormalOut = alphaMasked > 0
                    ? vec4(0.0, 0.0, 0.0, finalOpacity)
                    : vec4(normalize(mat3(view) * N), 0.0);

    // Auxiliary G-buffer: screen-space motion vector (.xy, un-jittered current
    // vs previous, UV units) for TAA reprojection, linear view-space Z (.z) for
    // GTAO position reconstruction, and effective perceptual roughness (.w) for
    // the tonemap specular-occlusion pass. Reconstructing occlusion positions
    // from a linear Z avoids the non-linear DEPTH24 buffer, whose grazing-angle
    // quantization staircases the reconstructed floor into AO banding.
    VelocityOut = vec4(screenVelocity(), ViewPos.z, roughnessMap);
    AlbedoOut = vec4(albedoMap, metallicMap);
    // SSS diffuse: subsurface-scaled skin diffuse (0 off-skin) in .rgb, this
    // material's scatter-profile index + 1 in .a (0 = non-skin, so the blur
    // rejects taps that cross into another material or the background). The SSS
    // post pass blurs .rgb and composites hdr + blur - this (diffuse softens,
    // FragColor's specular stays sharp), reading .a per pixel to select the
    // profile. Discarded unless the engine enables attachment 4 (sssEnabled).
    DiffuseOut = vec4(subsurface * sssDiffuse, float(max(sssProfileIndex + 1, 0)));

    // Weighted-blended OIT accumulate (guarded so oitPass 0 leaves FragColor and
    // the opaque/alpha-blend paths byte-identical): premultiplied color * depth
    // weight into AccumOut, alpha into RevealageOut. Indexed blend on the OIT FBO
    // sums the first (GL_ONE,GL_ONE) and multiplies (1 - alpha) into the second.
    if (oitPass > 0) {
        float w = oitWeight(-ViewPos.z);
        // Clamp the premultiplied weighted color to the fp16 ceiling (the accum
        // target is RGBA16F) so a bright near-camera translucent frag -- where the
        // depth weight nears its max -- can't write Inf and resolve to a white blob.
        AccumOut = vec4(min(color * finalOpacity * w, vec3(65504.0)), finalOpacity * w);
        RevealageOut = vec4(finalOpacity);
    }
}
