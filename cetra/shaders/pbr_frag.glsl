#version 330 core
// centroid, matching both vertex stages -- see pbr_vert for why. Without it a partly covered
// pixel is shaded from extrapolated attributes, which is where the MSAA-only specks came from.
in vec3 Normal;
in vec3 WorldPos;
in vec3 ViewPos;
in vec2 TexCoords;
in vec2 TexCoords2;  // UV1 for lightmaps/AO
centroid in vec4 VertexColor; // Vertex color (RGBA)
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
// OIT's two slots, bound only during an OIT sub-pass and carrying a different
// pair in each -- a different FBO and a different blend behind them:
//   accumulate (passMode 1): premultiplied colour * weight, and alpha (.r)
//   generation (passMode 2): the power moments b1..b4, and absorbance b0 (.r)
layout(location = 5) out vec4 AccumOut;
layout(location = 6) out vec4 RevealageOut;
// Ambient specular, split out for the post-chain occlusion composite (spec
// 11.4). Only lands when attachment 7 is enabled (split spec-occ); otherwise
// discarded.
//
// Its alpha is inert, and deliberately still mirrors FragColor's. The A2C rule
// is that coverage comes from the last active output that HAS an alpha channel,
// and attachment 7 is R11F_G11F_B10F -- three channels, so it is skipped and
// coverage falls to NormalOut. Measured: forcing this alpha to 1.0 moves 0
// pixels on a masked-hair frame where corrupting its RGB moves 88%, so the
// attachment is live and the alpha is not. Kept mirroring anyway, because the
// day the format gains an alpha channel this silently becomes load-bearing.
layout(location = 7) out vec4 SpecOut;

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
uniform vec3 attenuationColor;       // KHR_materials_volume, survivor at attenuationDistance
uniform float attenuationDistance;   // KHR_materials_volume, world units (0 = infinite: no absorption)
// Mipped opaque-scene resolve (unit 6). Also carries the moment atlas during the
// OIT accumulate -- see oitMomentTex below before narrowing or relocating this.
uniform sampler2D sceneColorTex;
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
// Anisotropic specular (roadmap B8). 0 leaves the isotropic highlight alone.
uniform float anisotropy;
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
// reads .g and metallic reads .b (glTF ORM); the rest read .r. Anisotropy is
// the one that is not a scalar mask: .rg carry a doubled-angle grain direction
// and .b a per-strand identity.
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

// Shadow mapping. The shared block (maps, matrices, cascade params, texel
// size) comes from the chunk; everything below is specific to this shader's
// per-fragment cascade selection and PCSS, which the catcher and particle
// consumers do not do. No CSM_OUTERMOST_PCF: they sample one scene-fit
// cascade, this one selects per fragment.
#include "csm.glsl"
// The receiver-plane bias policy both shadow lookups share.
#include "receiver_plane.glsl"
// ign() for the stochastic PCSS kernel rotation.
#include "noise.glsl"

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
// Stochastic PCSS: 1 = rotate the Poisson kernel per pixel and per frame.
// Set ONLY while TAA is accumulating -- the rotation trades the 16-tap
// banding for noise, and TAA is the only thing that averages that noise
// away (the shadow term is forward-shaded; it has no denoiser of its own).
uniform int pcssStochastic;
uniform int pcssFrameIndex; // Advances the per-frame rotation; frozen when off

// Perspective shadow maps for the punctual light types, one 2D layer each,
// separate from the directional cascade array. Each light carries its own base
// layer in its cluster entry (shadowMisc.y), so nothing here is per-type.
#include "punctual_shadow.glsl"

// Clustered-forward light blocks (spec 9.1): all analytic lights arrive
// through these UBOs -- directionals in a small unconditional array, the
// point/spot/area set via the per-fragment cluster index list.
#include "lights_ubo.glsl"
#include "view.glsl"
#include "velocity.glsl"
#include "fresnel.glsl"
#include "ltc.glsl"
// Indirect diffuse from a probe grid, when one is present (spec 9.7). Only the
// diffuse IBL term changes; specular, clearcoat and sheen stay on the env map.
#include "gi_volume.glsl"

// Cloud-deck sun occlusion on the ground (spec 11.41). The map rides the refraction
// sampler for the same forced reason the moment atlas does below -- see that comment
// for the driver rule. What differs is the exclusivity argument: this tenant is bound
// only in the opaque shading pass, where the other two provably are not, so it rests
// on the PASS rather than on material routing.
#define cloudShadowTex sceneColorTex
#include "cloud_shadow.glsl"

/*
 * The swash, for wet sand (spec 11.45). shore.glsl and NOT ocean.glsl: the ocean declares
 * nine samplers and this program has been at its sixteen since 4.10, where the run-up is a
 * closed form over scalar uniforms and costs a unit nothing.
 *
 * The clock is the one render.c publishes per program switch, which is the same value the
 * water surface reads -- so the sand cannot describe a different instant of the swash than
 * the sea is drawn at.
 */
#include "shore.glsl"
uniform float time;
// 0 = this material is never wetted, whatever the sea does. See Material.shore_wetness.
uniform float uShoreWetness;

/*
 * By-example stochastic albedo (spec 11.45). Declares no sampler of its own -- it re-reads
 * albedoTex, whose contents a material opting in has had transformed -- and keeps its inverse
 * table in uniform space, so it fits a program that has been at sixteen samplers since 4.10.
 */
#include "stochastic.glsl"

// Open porosity of sand that the swash fills, from Lagarde's 25-50% band for natural
// materials. What the diffuse albedo loses when the pores are full.
const float SHORE_POROSITY = 0.38;
// Lekner-Dorf internal reflection as a power on the albedo: saturating, so a channel the dry
// sand already absorbs is absorbed more, which is the cool-warm shift a wet beach has.
const float SHORE_DARKEN_POWER = 0.55;
// What a full film smooths the surface to, and the F0 of the air/water interface it puts in
// front of it. Water is LESS reflective than sand at normal incidence; the shine is the
// roughness, not the Fresnel.
const float SHORE_WET_ROUGHNESS = 0.12;
const float SHORE_WET_F0 = 0.02;
// How far a full film flattens the normal toward the geometric one. Not 1: a drained sheet
// still follows the sand it is lying on, and taking the normal all the way to flat reads as
// a decal of water rather than water on a beach.
const float SHORE_FLATTEN = 0.75;
// Stranded whitewater. Bright and rough -- entrained air, not a wet surface.
const float SHORE_FOAM_ALBEDO = 0.86;
const float SHORE_FOAM_ROUGHNESS = 0.85;

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
// The sheen environment: the same env convolved through the Charlie kernel
// (spec 10.7.1). Its unit is never probe-overridden, so sheen always reads
// the global environment.
uniform samplerCube charliePrefilteredMap;
uniform float maxCharlieLOD;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
// Uniform ambient radiance (cd/m^2) for scenes with no IBL. A real emitter, so
// it does NOT track exposure or light magnitude; zero means unlit is black.
uniform vec3 ambientRadiance;
uniform float maxReflectionLOD;
// Multi-scatter energy compensation toggle (inert unless iblEnabled)
uniform int energyCompEnabled;
uniform int clearcoatEnabled; // Global clearcoat lobe toggle (--no-clearcoat)
uniform int specularEnabled;  // Global KHR_materials_specular toggle (--no-specular)
uniform int sheenEnabled;     // Global KHR_materials_sheen toggle (--no-sheen)
uniform int parallaxEnabled;  // Global POM toggle (--no-parallax, §4.11)
// Which pass is drawing, and therefore where this shader stops: 0 shade,
// 1 OIT accumulate, 2 OIT moment generation, 3 depth prepass. Mirrors
// SubmitPass in common.h, so the numbering is load-bearing.
//
// The depth prepass is a VALUE here rather than a flag beside this one, and the
// reason is that the states are mutually exclusive: nothing is a moment draw and
// a depth-only draw at once. As two uniforms that pairing was expressible and
// its behaviour fell out of which early return happened to be written first.
uniform int passMode;
// 1 = weight the accumulate by measured transmittance rather than the depth
// curve. Orthogonal to which pass is drawing, and separate from passMode for
// that reason; oit_resolve_frag carries the same bit under the same name.
uniform int oitMomentWeighted;
// The moment atlas arrives through the refraction sampler rather than one of
// its own, and that is forced rather than clever: the driver counts DECLARED
// fragment samplers against the 16-unit limit, not distinct units, so a second
// uniform pointing at unit 6 is still a seventeenth sampler and fails to link.
// This shader declares exactly sixteen.
//
// The two tenants cannot collide, and for a structural reason rather than a
// convention: the accumulate sub-pass draws only non-transmissive meshes, and
// sceneColorTex is read only where transmission > 0.
#define oitMomentTex sceneColorTex
// Generation is a mode in this shader rather than a program of its own on
// purpose. A separate program would have to reproduce the UV transform, POM, the
// mask-array opacity chain and the Fresnel to reach the same finalOpacity the
// accumulate writes, and the moments have to describe exactly that opacity --
// a drift there mis-weights every layer silently, where a mode cannot drift at
// all. The cost a mode would otherwise carry, shading a fragment whose colour is
// discarded, is paid off by the early-out in main.
uniform vec2 oitMomentInvSize; // Reciprocal atlas size (twice the frame's height)
uniform vec2 oitNearFar; // Camera near/far, the interval the depth warp spans
// 1 = route ambient specular to SpecOut instead of FragColor. 0 (the GLSL
// uniform default) is the inline path, so a draw that never sets it -- a
// capture, the late/OIT passes, every non-split mode -- keeps its specular
// energy in color even where attachment 7 is not bound.
uniform int splitAmbientSpec;
uniform int sssEnabled;       // Global separable-SSS toggle (--no-sss)
uniform float subsurface;     // Per-material SSS strength (0 = off; also the skin flag)
uniform int sssProfileIndex;  // This material's scatter-profile slot; written into DiffuseOut.a so
                              // the screen-space blur can look up the profile per pixel
#define MAX_SSS_PROFILES 8 // mirror of postfx.h MAX_SSS_PROFILES
uniform int skinPreintEnabled;  // Global pre-integrated skin toggle (--no-skin-preint)
uniform float curvatureScale;   // Per-material pre-integration strength (0 = off)
uniform float sssMaxScatterPerDepth; // World scatter the SSS blur can still reach per unit view
                                     // depth; its kernel is capped in pixels, so past a certain
                                     // closeness it under-delivers the authored radius
uniform vec4 sssProfiles[MAX_SSS_PROFILES]; // rgb = per-channel scatter weight, w = world radius
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


// Firefly ceiling on the BRDF -- dimensionless, so it is independent of both
// light magnitude and exposure. That independence is the whole point; a ceiling
// on the shaded PRODUCT is a number of nits, and no fixed number of nits is
// correct at two light magnitudes or two exposures (spec 10.1 phase 5).
//
// Diffuse peaks at albedo/PI <= 1/PI, so this only ever touches specular. The
// value is the GGX peak D/4 at roughness ~0.09: everything rougher passes
// untouched, and the sub-pixel spikes sharper than that -- which is what aliases
// into white sparkle on normal-mapped metal -- get cut.
const float BRDF_MAX = 1000.0;

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

// KHR_materials_specular's dielectric fresnel: mix(f0, f90, (1-cos)^5).
// Called ONLY inside specExt guards -- the default path never routes
// through it, which is what keeps non-carrying materials byte-identical.
vec3 fresnelSchlickF90(float cosTheta, vec3 f0, vec3 f90) {
    return mix(f0, f90, pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0));
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

#include "sheen.glsl"
#include "preintegrated_skin.glsl"

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

// getDistanceAtt lives in include/lights_ubo.glsl, beside spotConeFactor and the
// packed field it decodes.

// 16-tap Poisson disk (unit radius) for the PCSS blocker search and filter.
// Two sampling regimes, chosen by pcssStochastic:
//
// UNROTATED (the deterministic default): a per-pixel rotation decorrelates
// the pattern between neighbours, which turns the 16-tap quantization into
// per-pixel shadow noise — invisible on diffuse but riding the sharp
// specular lobe into a field of bright speckle on the metal. A coherent disk
// gives a smooth penumbra instead, at the price of the tap pattern itself
// becoming visible as stepped silhouette ghosts when a wide penumbra is
// viewed at macro distance.
//
// ROTATED (pcssStochastic): per-pixel AND per-frame rotation, admissible
// exactly when a temporal accumulator is running to average the noise it
// trades the banding for — the C side sets the flag only while TAA
// accumulates. The speckle that killed static rotation is the time-average's
// variance, and TAA converges it away.
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

// No normal-offset bias here, and that is a tried-and-rejected choice rather
// than an oversight. Offsetting the lookup along N is the textbook cure for a
// texel-wide leak along a silhouette. Measured on these fixtures it lost twice:
// on the directional path it lifted the rock pile's ground contacts off the
// spheres casting them (14.6k pixels moved, no artifact removed), and at a
// concave corner it made things worse, pushing the sample into the open space
// the map reports as lit and widening one bright line into a band of steps.
// The receiver-plane bias below carries it instead.

// Receiver-plane bias for the cascades (spec 10.5; policy shared with the
// punctual lookup via receiver_plane.glsl). The cascade projection is
// orthographic, so the perspective machinery collapses: the shadow-space
// derivative of the receiver per screen axis is one linear transform of the
// world-space derivatives -- no divide, no w guards, constant across a planar
// receiver. The clip->[0,1] remap's 0.5 is omitted: a uniform power-of-two
// scale of px/py cancels bit-exactly through the gradient's ratio, so only
// the whole-vector scale matters, and none is cheapest.
//
// Blocker classification separation, relative on linearized ortho depth: a
// blocker must be genuinely NEARER the light than the receiver's plane, not
// merely within filter-bias distance of it -- a receiver sits within filter
// bias of itself, which 10.4 measured dragging the average blocker depth and
// collapsing the penumbra to a third of its width. Relative means the
// required separation scales with the cascade's full depth range (eye
// pushback included) -- deliberate, so one constant serves every fit; do not
// "fix" it into an absolute epsilon without re-running the gates.
#define CSM_BLOCKER_SEPARATION 0.99

vec2 csmPlaneGradient(int layer, vec3 ddxWorld, vec3 ddyWorld) {
    return receiverPlaneGradient(mat3(lightSpaceMatrix[layer]) * ddxWorld,
                                 mat3(lightSpaceMatrix[layer]) * ddyWorld);
}

// Fixed 3x3 PCF, plane-biased per tap.
// layer is a shadow-array layer (slot * cascadeCount + cascade).
float shadowPCF3x3(int layer, vec2 uv, float currentDepth, vec2 duv_dz) {
    float ref = currentDepth - SHADOW_PLANE_BIAS_FLOOR;
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            float pcfDepth = texture(shadowMaps, vec3(uv + offset, float(layer))).r;
            shadow += ref + receiverPlaneBias(duv_dz, offset) > pcfDepth ? 1.0 : 0.0;
        }
    }
    return 1.0 - (shadow / 9.0);
}

// Contact-hardening soft shadow (PCSS). lightSize is the scalar emitter size
// (world units of the emitter disk); larger = softer, faster-growing penumbra
// with blocker distance. The algorithm is isotropic, so callers collapse a
// rectangular emitter to a single dimension.
// One cascade layer's shadow estimate for a projected position already known
// to be in bounds: PCSS when enabled and the emitter resolves, else 3x3 PCF.
// The OPAQUE half: what the depth cascades (or the moment array in their place)
// say about light reaching this point. cascadeShadowTap below folds in the
// translucent half.
float cascadeOpaqueTap(int layer, vec3 projCoords, vec2 duv_dz, float lightSize) {
    // One tap, ahead of BOTH branches below: neither the kernel nor the
    // receiver-plane bias applies, because the moments already carry the depth
    // spread inside the footprint that those exist to cope with. Placed here
    // rather than relying on the app clearing pcssEnabled, so a GUI toggle
    // cannot put PCSS on a map whose .r is a mean depth and not a depth.
    if (msmEnabled == 1)
        return 1.0 - csmMomentOcclusion(layer, projCoords.xy, projCoords.z);

    float currentDepth = projCoords.z;

    if (pcssEnabled == 0) {
        return shadowPCF3x3(layer, projCoords.xy, currentDepth, duv_dz);
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
        return shadowPCF3x3(layer, projCoords.xy, currentDepth, duv_dz);
    }

    float ref = currentDepth - SHADOW_PLANE_BIAS_FLOOR;

    // Stochastic kernel rotation (see the POISSON16 comment), built ONCE so
    // the blocker estimate and the filter read the same rotated taps -- if
    // they disagreed, the penumbra radius would decorrelate from the
    // occlusion it filters. Identity when off: no trig runs, and the
    // identity multiply is IEEE-exact, so the deterministic image is
    // bit-identical to the unrotated constants.
    mat2 rot = mat2(1.0);
    if (pcssStochastic == 1) {
        vec2 fc = gl_FragCoord.xy + vec2(float(pcssFrameIndex) * 5.588238);
        float ang = 6.2831853 * ign(fc);
        float c = cos(ang);
        float s = sin(ang);
        rot = mat2(c, s, -s, c);
    }

    // 1. Blocker search: average depth of texels genuinely nearer the light
    //    than the receiver's PLANE at each tap, over a disk the size of the
    //    emitter's shadow footprint. Plane-relative AND separation-gated,
    //    because the receiver's own surface is in the map: at grazing its
    //    stored depth at a far tap differs from the fragment's by the plane
    //    slope, and a per-tap filter bias is the wrong scale for "what is
    //    occluding me" -- a receiver sits within filter bias of itself.
    float zReceiver = linearizeOrthoDepth(currentDepth, nearFar);
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = rot * POISSON16[i] * lightSizeUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(layer))).r;
        float zTap = linearizeOrthoDepth(d, nearFar);
        float zPlane =
            linearizeOrthoDepth(currentDepth + receiverPlaneBias(duv_dz, off), nearFar);
        if (zTap < zPlane * CSM_BLOCKER_SEPARATION) {
            blockerSum += zTap;
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

    // 3. Variable-width PCF over the same disk, plane-biased per tap
    float shadow = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = rot * POISSON16[i] * filterRadiusUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(layer))).r;
        shadow += ref + receiverPlaneBias(duv_dz, off) > d ? 1.0 : 0.0;
    }
    return 1.0 - (shadow / 16.0);
}

// `ddxWorld`/`ddyWorld` are the screen-space derivatives of the world
// position, hoisted by the caller. The hoist is mandatory, not a convenience:
// this runs inside the fused light loop, whose trip count is per-fragment
// (clusterCount), so a local dFdx here is undefined.
// Opaque occlusion times translucent transmittance (spec 11.26).
//
// A product, not a min: the two caster sets are disjoint by construction -- a
// mesh is represented by one map or the other, never both -- so they are
// independent attenuations of the same light ray. The return stays a scalar,
// which is what keeps every consumer of this value unchanged.
float cascadeShadowTap(int layer, vec3 projCoords, vec2 duv_dz, float lightSize) {
    float visibility = cascadeOpaqueTap(layer, projCoords, duv_dz, lightSize);
    if (tsmEnabled == 1)
        visibility *= csmTransmittance(layer, projCoords.xy, projCoords.z);
    return visibility;
}

float calculateShadow(int shadowIndex, int cascade, vec3 worldPos, float lightSize,
                      vec3 ddxWorld, vec3 ddyWorld) {
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

        // Per layer: each cascade has its own matrix, so its own gradient. Not
        // computed on the moment path, which never biases a tap -- two mat3
        // products and a divide per cascade per light per fragment otherwise.
        vec2 duv_dz =
            msmEnabled == 1 ? vec2(0.0) : csmPlaneGradient(layer, ddxWorld, ddyWorld);
        visibility = min(visibility, cascadeShadowTap(layer, projCoords, duv_dz, lightSize));
        if (visibility <= 0.0)
            break; // fully occluded; wider maps cannot add more
    }
    return visibility;
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

#include "mboit.glsl"

// Alpha carries two unrelated quantities and only one of them is a Fresnel
// event:
//
//   coverage        how much of this pixel has geometry in it, authored per
//                   texel. A hair card's alpha is a stencil -- "there is no
//                   hair here".
//   materialOpacity how much light the SURFACE transmits, a scalar. A visor is
//                   20% opaque everywhere.
//
// A dielectric reflects nearly everything at grazing incidence, so less
// background transmits and a translucent surface really does go opaque -- that
// is the mix() below, and it is the complement to the specular Fresnel rather
// than a duplicate of it. But where coverage is zero there is no interface, so
// there is no Fresnel event to have; lifting coverage invents surface the
// artist deleted. Hence the split: the boost scales the scalar, coverage rides
// through untouched, and opacity can never exceed coverage.
//
// Shared by the shading path and the moment-generation early-out, and that
// sharing is the point -- the moments summarise WHERE the opacity is, so they
// have to be built from the same number the accumulate later writes. Two copies
// of this expression would mis-weight every layer without failing anything.
float fresnelOpacity(float coverage, float materialOpacity, float iorF0, float NdotV) {
    if (materialOpacity >= 1.0)
        return coverage;
    float f = iorF0 + (1.0 - iorF0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
    return coverage * mix(materialOpacity, 1.0, f);
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
    if (renderMode == 12) {
        /*
         * How far a varying left the range its own three vertices bound.
         *
         * Under MSAA this stage runs once per PIXEL while coverage is per SAMPLE, so a varying
         * is evaluated at one point -- the pixel centre, unless it is qualified `centroid`. On a
         * partly covered pixel that centre can sit outside the triangle, and interpolating
         * outside a triangle is extrapolation: the barycentric weights go negative and the
         * result leaves the convex hull of the three vertex values.
         *
         * R and G are EXACT tests, on any asset, with nothing to calibrate. The vertex stage
         * normalizes both vectors, so every vertex value is unit -- and a convex combination of
         * unit vectors has length at most one, by the triangle inequality, with equality only
         * when they are all equal. A length above one therefore PROVES a negative weight; no
         * other input can produce it. Rounding cannot fake it either: fp32 error on a length is
         * ~1e-7 and the gain below needs 6e-5 before the 8-bit write sees anything.
         *
         * TBN[1] gets no channel, deliberately. It is cross(N, T) * w, whose length at a vertex
         * is the sine of the angle between N and T -- at most one, but not one, so the same
         * test would report authored non-perpendicularity as extrapolation.
         *
         * B is the one channel that is FIXTURE-SCOPED, and the difference is not a detail:
         * nothing bounds a UV except what the mesh authored, so leaving [0,1] is evidence only
         * where the mesh authored [0,1]. On anything tiling, leaving [0,1] is the normal case
         * and this channel means nothing.
         *
         * The gain is load-bearing rather than cosmetic. A debug frame still takes the MSAA
         * resolve, which is a box filter, so an excursion arrives scaled by its own coverage --
         * and the excursion is largest exactly where the coverage is smallest. Unscaled, that
         * product lands under one 8-bit code.
         */
        const float EXTRAP_GAIN = 64.0;
        vec2 uvOut = max(-TexCoords, TexCoords - 1.0);
        vec3 excursion =
            vec3(length(Normal) - 1.0, length(TBN[0]) - 1.0, max(uvOut.x, uvOut.y));
        FragColor = vec4(clamp(excursion * EXTRAP_GAIN, 0.0, 1.0), 1.0);
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
        // coverage * materialOpacity, and no Fresnel: this view answers "what is
        // authored here", so lifting it at grazing would be the wrong question.
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
        if (stochasticScale > 0.0) {
            /*
             * The map is stored NON-sRGB on this path, and the decode happens here instead.
             *
             * The histogram transform and its inverse table are both defined on the stored
             * codes, so a hardware sRGB decode sitting between the two would put the blend in
             * a different space from the one the histogram was measured in -- the inverse
             * would then be undoing a transform of a curve of the values it was built from.
             * Sample, blend, invert, and only then go to linear.
             *
             * No alpha: the transform is per channel over rgb, and a material wanting cutout
             * foliage wants an untransformed map anyway.
             */
            albedoMap = albedo * sRGBToLinear(stochasticSample(albedoTex, uv, uv));
        } else {
            // sRGB texture: the hardware already decoded the sample to linear
            vec4 albedoSample = texture(albedoTex, uv);
            albedoMap = albedo * albedoSample.rgb;
            texAlpha = albedoSample.a;
        }
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

    // The prepass is done here whenever the answer is binary. Surviving the test
    // above IS the depth decision, and with one sample per pixel there is no
    // coverage left to compute -- no output is read, since the pass draws with
    // colour writes masked off.
    //
    // Under alpha-to-coverage the alpha additionally chooses WHICH samples take
    // depth, so that path cannot stop here; it runs on to the exit beside the
    // moment generation, where finalOpacity exists. That is the whole reason
    // this is two exits rather than one: the shipping AA path (TAA, one sample)
    // never pays for a coverage number nothing will read.
    if (passMode == 3 && alphaToCoverage == 0)
        return;

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

    // Geometric coverage, kept apart from the scalar translucency it used to be
    // multiplied into (see fresnelOpacity). A dedicated opacity map and an
    // alpha-cut albedo are both stencils, so they COMBINE -- the map used to
    // replace texAlpha outright, which silently dropped the cutout on any
    // material carrying both.
    float coverage = texAlpha;
    if (opacityLayer >= 0)
        coverage *= texture(maskArray, vec3(uv, float(opacityLayer))).r;
    float opacity = coverage * materialOpacity;

    // Microsurface detail - modulates roughness for fine surface detail
    if (microsurfaceLayer >= 0) {
        float detail = texture(maskArray, vec3(uv, float(microsurfaceLayer))).r;
        roughnessMap = clamp(roughnessMap * (0.5 + detail), 0.04, 1.0);
    }

    // Anisotropy - for brushed metal, hair effects
    // Grain direction and how confident the map is about it. The layer stores a
    // coherence-weighted DOUBLED angle, which is the only form that survives the
    // mask array's resample and mip chain -- a direction and its reverse are the
    // same grain, so raw vectors would average to nothing under minification.
    // Halving the angle on read returns the +T half-plane representative.
    vec2 anisoDir = vec2(1.0, 0.0);
    float anisoCoherence = 0.0;
    if (anisotropyLayer >= 0) {
        vec3 texel = texture(maskArray, vec3(uv, float(anisotropyLayer))).rgb;
        vec2 doubled = texel.rg * 2.0 - 1.0;
        anisoCoherence = min(length(doubled), 1.0);
        float grain = 0.5 * atan(doubled.y, doubled.x);
        anisoDir = vec2(cos(grain), sin(grain));
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

    /*
     * WET SAND: what the swash left behind (spec 11.45).
     *
     * Sand a wave has just run over is darker, smoother and slightly shinier than dry sand,
     * and it dries from the top down over the following seconds. The run-up that decides
     * where the water reached is a closed form of position and time -- see shore.glsl -- so
     * this costs no sampler, reads no history, and is exactly the same edge the water surface
     * is drawn against, which is what makes the two agree at the waterline.
     *
     * Everything above the run-up's own analytic ceiling exits before a single tap runs, so
     * the cost is confined to the swash band and dry geometry pays one compare.
     */
    // How much free water is lying on the surface, carried down to the Fresnel below: F0 is
    // derived from `ior` further on, so the film cannot be folded in at this point.
    float shoreFilm = 0.0;
    if (uShoreWetness > 0.0 && waterSurfHeight > 0.0) {
        float h = WorldPos.y - waterLevel;
        // Nothing outside the bed's domain has a shore, and without this a material that
        // opted in would be wetted by geometry that merely sits low a kilometre inland.
        float inRange = 1.0 - smoothstep(waterExtent * 0.75, waterExtent, length(WorldPos.xz));
        if (h < shoreEdgeCeiling() && inRange > 0.0) {
            ShoreSwash sw = shoreSwash(WorldPos.xz, h, time);
            float amount = uShoreWetness * inRange;
            float wet = sw.wet * amount;
            float film = sw.film * amount;

            /*
             * POROSITY DARKENING (Lagarde). Water filling the pores raises the effective
             * refractive index between the grains, so less light escapes back out: the
             * diffuse albedo drops by the open porosity that got filled. 0.38 sits inside
             * the 25-50% band that work gives for natural materials.
             *
             * A tint multiply was the obvious alternative and is wrong twice over -- it does
             * not saturate, and it makes wet sand a colour rather than a darker version of
             * whatever colour the sand already is.
             */
            albedoMap *= 1.0 - SHORE_POROSITY * wet;

            /*
             * THE HUE SHIFT, and not by Beer-Lambert. Water's absorption is
             * (0.0035, 0.0004, 0) per cm, so a film 0.1-1 mm thick transmits exp(-0.0007) --
             * 0.9993, which is nothing. This session shipped and reverted exactly that
             * mistake once already on the sea's capillary slope term.
             *
             * What actually reddens wet sand is Lekner-Dorf internal reflection: light that
             * scatters inside the grain bed is reflected back INTO it at the water surface
             * instead of escaping, so it makes more passes through the sand's own pigment.
             * A power law on the albedo is the saturating form of that -- a channel the dry
             * sand already absorbs gets absorbed more -- and it composes with the porosity
             * term above rather than fighting it.
             */
            albedoMap = pow(albedoMap, vec3(1.0 + SHORE_DARKEN_POWER * wet));

            /*
             * The film makes the surface SMOOTH, which is why it looks shinier -- not more
             * reflective. An air/water interface is F0 0.02 against sand's 0.04, so the
             * reflectance actually falls; what rises is how tightly it is concentrated.
             */
            roughnessMap = clamp(mix(roughnessMap, SHORE_WET_ROUGHNESS, film), 0.04, 1.0);
            shoreFilm = film;
            // A drained sheet reads as a sheet: the ripples the normal map carries are under
            // the water, not on it.
            N = normalize(mix(N, normalize(Normal), film * SHORE_FLATTEN));

            /*
             * Whitewater the tongue stranded. It is drawn HERE rather than by the water
             * because our water surface is discarded above the line the column closes -- so
             * once the swash retreats there is no water left to carry it, and foam on the
             * sand has to be the sand's business.
             */
            float foam = sw.foam * amount;
            albedoMap = mix(albedoMap, vec3(SHORE_FOAM_ALBEDO), foam);
            roughnessMap = clamp(mix(roughnessMap, SHORE_FOAM_ROUGHNESS, foam), 0.04, 1.0);
        }
    }

    // Screen-space derivatives of the world position, taken HERE because the
    // punctual shadow lookup that needs them runs inside the clustered light
    // loop, and derivatives are undefined in non-uniform control flow. They
    // reconstruct the receiver's own plane per light (punctual_shadow.glsl).
    vec3 ddxWorld = dFdx(WorldPos);
    vec3 ddyWorld = dFdy(WorldPos);

    // Calculate view direction (WorldPos -- world space, as the maths needs)
    vec3 V = normalize(camPos - WorldPos);

    // Render modes that need texture data
    if (renderMode == 7) {
        // Simple Diffuse Lighting (same fused dir+cluster fetch as the PBR loop)
        vec3 Lo = vec3(0.0);
        uvec2 sList = clusterLightList(gl_FragCoord.xy, -ViewPos.z);
        uint sOffset = sList.x;
        int sCount = int(sList.y);
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
                vec3 toLight = lightPos - WorldPos;
                L = normalize(toLight);
                attenuation = getDistanceAtt(dot(toLight, toLight),
                                             clusterLights[li].attenCutoff.x);
                attenuation *=
                    spotConeFactor(clusterLights[li].dirType.w, clusterLights[li].dirType.xyz,
                                    clusterLights[li].attenCutoff.w,
                                    clusterLights[li].shadowMisc.x, L);
                lightCI = clusterLights[li].colorIntensity.xyz;
            }
            float NdotL = max(dot(N, L), 0.0);
            Lo += albedoMap * lightCI * attenuation * NdotL;
        }
        // Display-relative ambient, and legitimately so: this debug mode does
        // its own Reinhard and sRGB two lines down, and debug frames take the
        // passthrough blit rather than the post chain, so nothing here reaches
        // the HDR buffer or the auto-exposure meter. Not the same constant as
        // the shading path's old 3%-of-white floor, which did (spec 10.1).
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
    // A water film puts an air/water interface in front of the surface, and water's F0 is
    // 0.02 against a dielectric's 0.04 -- so wet sand reflects LESS at normal incidence, not
    // more. What makes it read shiny is the roughness drop above, which concentrates the
    // same energy instead of adding any.
    iorF0 = mix(iorF0, SHORE_WET_F0, shoreFilm);
    vec3 F0 = vec3(iorF0);
    // KHR_materials_specular re-parameterizes the DIELECTRIC Fresnel, per the
    // spec's exact form (10.8): f0 = min(iorF0 * specularColor, 1) * specular
    // (clamp BEFORE the factor), f90 = specular, fresnel = mix(f0, f90,
    // (1-|VdotH|)^5), and the diffuse trade is the ACHROMATIC
    // 1 - max_value(fresnel). Everything lives behind the -1 sentinel as
    // guarded overwrites: non-carrying materials compile the exact original
    // expressions -> byte-identical (the direct-weight formulation was tried
    // in 4.10.1 and reverted -- compiler reassociation broke the cmp gate).
    // specDielectricF0 is kept PRE-metal-mix: the extension is
    // dielectric-only, so every guarded overwrite blends back to the
    // standard form by metallicMap.
    bool specExt = specularEnabled > 0 && specularFactor >= 0.0;
    vec3 specDielectricF0 = F0;
    if (specExt) {
        specDielectricF0 = min(F0 * specularColorFactor, vec3(1.0)) * specularFactor;
        F0 = specDielectricF0;
    }
    F0 = mix(F0, albedoMap, metallicMap);

    float NdotV = max(dot(N, V), 0.0);

    // The opacity every exit below writes, computed ONCE. Three passes used to
    // call fresnelOpacity with the same four arguments at three points in this
    // function, agreeing only by inspection -- and the moments have to describe
    // exactly the opacity the accumulate later writes, which is the drift
    // fresnelOpacity's own comment says two copies would hide. Nothing between
    // here and the last use reassigns any input.
    float finalOpacity = fresnelOpacity(coverage, materialOpacity, iorF0, NdotV);

    // Moment generation ends here. It needs opacity and depth and nothing else:
    // it is a summary of WHERE the opacity is, never of what colour it is, so
    // everything below -- the clustered light loop and its shadow taps, IBL,
    // area lights, SSS, clearcoat, sheen -- would be shaded and thrown away. The
    // driver cannot drop it on its own, because FragColor is written
    // unconditionally at the end.
    //
    // Legal to leave the other outputs unwritten: the generation FBO binds only
    // locations 5 and 6. passMode is a uniform, so the branch is coherent.
    if (passMode == 2) {
        float absorbance =
            mboitAbsorbance(finalOpacity);
        AccumOut = mboitMoments(absorbance, mboitWarpDepth(-ViewPos.z, oitNearFar));
        RevealageOut = vec4(absorbance);
        return;
    }

    // The prepass's alpha-to-coverage exit, reached only when A2C is live -- the
    // binary case left at the cutoff. Written to the SAME output the shading
    // pass derives coverage from, and for the same reason recorded at NormalOut
    // below: this driver takes A2C from the last colour output carrying an alpha
    // channel, not from output 0. Feeding it a different number here would give
    // the two passes different sample masks and GL_LEQUAL would then delete the
    // samples they disagreed on.
    //
    // FragColor is written too, and is not redundant: it is the last
    // alpha-bearing output when the normals G-buffer is off this frame.
    if (passMode == 3) {
        FragColor = vec4(0.0, 0.0, 0.0, finalOpacity);
        NormalOut = vec4(0.0, 0.0, 0.0, finalOpacity);
        return;
    }

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
        // The LUT is always bound now, so this gate is a SCOPE bound, not a
        // validity one: it keeps no-environment renders byte-identical to
        // the pre-E-LUT path. Energy compensation applies to ANALYTIC
        // specular too, so hoisting this fetch and the comp out of the gate
        // is a real follow-up -- one that moves no-env pixels and wants its
        // own measurement. The ambient block below reuses this fetch (same
        // coordinates).
        brdf = texture(brdfLUT, vec2(NdotV, roughnessMap)).rg;
        float Ess = brdf.x + brdf.y;
        if (energyCompEnabled > 0 && Ess > 1e-4) {
            energyComp = 1.0 + F0 * (1.0 / Ess - 1.0);
        }
    }

    // Sheen constants are per-fragment (only D*V varies per light): resolve
    // color, roughness, and the LUT's Charlie directional albedo E once,
    // ahead of both the light loop and the ambient block. The albedo scaling
    // is the spec's simplified VdotN-only form,
    // 1 - max3(sheenColor) * E(NdotV); the per-light min with E(LdotN) is
    // deliberately skipped. E sits in the engine-owned LUT's blue channel,
    // bound for every scene, so this reads correctly with no environment.
    bool sheenActive = sheenEnabled > 0 && maxComp(sheenColorFactor) > 0.0;
    vec3 sheenColorPx;
    float sheenRough;
    float sheenE;
    float sheenScale = 1.0; // read unconditionally in the per-light accumulate
    if (sheenActive) {
        sheenColorPx = sheenColorAt(uv);
        sheenRough = clamp(sheenRoughnessFactor, SHEEN_MIN_ROUGHNESS, 1.0);
        sheenE = texture(brdfLUT, vec2(NdotV, sheenRough)).b;
        sheenScale = 1.0 - maxComp(sheenColorPx) * sheenE;
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

    // Pre-integrated skin (§11.13). Every term of the gate is a uniform, so the
    // branch is uniform across the draw: the derivatives inside are well-defined
    // (as at specularAAStrength above) and a non-skin draw pays nothing.
    //
    // sssProfileIndex >= 0 is not belt-and-braces: the skin buffer is tagged
    // with profile + 1, so an unassigned -1 writes the tag reserved for "not
    // skin" and the blur rejects those pixels. A material the blur already
    // ignores must not get pre-integration either, or the two disagree. It is
    // NOT what keeps foliage out -- tree leaves and grass carry both a
    // subsurface weight and a real profile. curvature_scale defaulting to 0 is
    // the only thing holding that line.
    // Deliberately NOT gated on sssEnabled. Pre-integration is a shading model,
    // not a screen-space pass: with the blur off there is no hdr + blur(D) - D
    // to cancel against, so putting the falloff in Lo alone is simply correct.
    // That also makes --no-sss the way to measure this term on its own, which
    // is what the curvature gate needs -- and a scene can carry curvature
    // without paying for the blur.
    bool skinPreint = skinPreintEnabled > 0 && curvatureScale > 0.0 && subsurface > 0.0 &&
                      sssProfileIndex >= 0;
    SkinShape skinShapeF;
    if (skinPreint) {
        vec4 prof = sssProfiles[clamp(sssProfileIndex, 0, MAX_SSS_PROFILES - 1)];
        // Curvature from the GEOMETRIC normal. N here is post-normal-map, and
        // its derivative is a texture-detail metric -- see preintegrated_skin.
        float curv = skinCurvature(normalize(Normal), ddxWorld, ddyWorld);
        skinShapeF = skinShape(skinSigma(prof.rgb, prof.w, curv, curvatureScale,
                                         sssMaxScatterPerDepth, -ViewPos.z));
    }

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
    uvec2 clusterList = clusterLightList(gl_FragCoord.xy, -ViewPos.z);
    uint clusterOffset = clusterList.x;
    int clusterCount = int(clusterList.y);
    int numDir = lightCounts.x;

    // LTC tables (spec 9.2). Both lookups depend only on this fragment's
    // roughness and view angle, so they hoist out of the light loop: the
    // per-light work is just the quad integral. Gated on the scene's area
    // count, which is uniform across the draw, so a scene without panels
    // never touches the LUTs. The initializers are dead values, present only
    // because the compiler cannot see that this gate and the area test in the
    // loop agree; nothing reads them.
    mat3 ltcMinv = mat3(1.0);
    vec2 ltcAmp = vec2(0.0);
    if (lightCounts.z > 0) {
        // Roughness floor (LTC_MIN_ROUGHNESS). Below it the fitted Minv grows
        // extreme enough to collapse the transformed quad corners toward each
        // other, and the edge integral's cross(v1,v2) * theta/sin(theta) turns
        // into a 0 * inf that fp32 cannot resolve -- it lands on the 1e-7 clamp
        // in ltcIntegrateEdgeVec and produces a structured radial fan instead
        // of a highlight. Empirical: measured against a 7:1 sliver panel, 0.08
        // still speckles, 0.10 leaves a trace, 0.12 is clean. NOT a proof --
        // a thinner panel degenerates the quad further and can still break
        // this. Mirror-sharp reflection is SSR/probe work, not LTC's.
        vec2 ltcUV = ltcCoords(max(roughnessMap, LTC_MIN_ROUGHNESS), NdotV);
        ltcMinv = ltcMatrix(ltcUV);
        // .zw are meaningless at THIS uv -- the sphere form factor in .w is
        // indexed separately inside ltcPanel (see ltc.glsl)
        ltcAmp = textureLod(ltcTex, vec3(ltcUV, LTC_LAYER_AMP), 0.0).xy;
    }

    // Anisotropic shading frame, resolved once: it depends on the SURFACE, not
    // on any light, and the loop below runs per light on geometry (hair cards,
    // brushed panels) that overdraws heavily.
    //
    // Aniso is the strength actually used, so the loop tests one value rather
    // than re-deriving the gate. Without a map it stays 0 and the isotropic
    // highlight below is untouched -- a quad has one tangent, and stretching a
    // highlight along it would be a claim about the card rather than about what
    // is painted on it.
    vec3 anisoT = T, anisoB = B;
    float aniso = 0.0;
    if (anisotropy > 0.0 && anisotropyLayer >= 0) {
        vec3 dir = normalize(anisoDir.x * T + anisoDir.y * B);
        // Blended by coherence: where the map is unsure the card tangent stands,
        // and the strength falls off with it so an averaged-to-nothing
        // neighbourhood goes isotropic rather than picking a direction at random.
        anisoT = normalize(mix(T, dir, anisoCoherence));
        anisoB = normalize(cross(N, anisoT));
        aniso = anisotropy * anisoCoherence;
    }

    for (int k = 0; k < numDir + clusterCount; k++) {
        vec3 L;
        float attenuation;
        vec3 lightCI;   // color * intensity (premultiplied on CPU)
        vec2 lightSize; // PCSS emitter size (directional/spot) or panel extent (area)
        int dirShadowSlot = -1;
        int punctualLayer = -1; // Base layer in the punctual array, -1 = no map

        if (k < numDir) {
            L = normalize(-dirLights[k].dirShadow.xyz);
            attenuation = 1.0;
            lightCI = dirLights[k].colorIntensity.xyz;
            lightSize = dirLights[k].sizeMisc.xy;
            dirShadowSlot = int(dirLights[k].dirShadow.w);
        } else {
            uint li = lightIndexAt(clusterOffset + uint(k - numDir));
            vec3 lightPos = clusterLights[li].posRange.xyz;

            // Area panels (spec 9.2) integrate the whole rectangle
            // analytically rather than treating it as a point, so they take
            // none of the point/spot path -- neither the attenuation below
            // nor the Cook-Torrance body. That also implements the v1 limits
            // by construction: no shadows, clearcoat, sheen, SSS or POM from
            // a panel. A panel can only ever arrive through the cluster list,
            // which is why this test lives here and not after the branch.
            if (clusterLights[li].dirType.w == 3.0) {
                vec2 ff = ltcPanel(N, V, WorldPos, ltcMinv, lightPos,
                                   clusterLights[li].dirType.xyz,
                                   clusterLights[li].upArea.xyz,
                                   clusterLights[li].shadowMisc.zw * 0.5);

                // See the normalization contract in ltc.glsl: no 2*pi, no 1/pi
                vec3 areaSpec = (F0 * ltcAmp.x + (1.0 - F0) * ltcAmp.y) * ff.y;
                // KHR_materials_specular: ltcAmp packs (integral DV,
                // integral pow5*DV), so the generic Schlick form is
                // f0*ampx + (f90 - f0)*ampy. The clamp-before-factor order
                // guarantees f0 <= factor componentwise, so the grazing
                // coefficient is non-negative by construction. Pure
                // dielectric vs pure metal, per the analytic site.
                if (specExt) {
                    vec3 areaExt = (specDielectricF0 * ltcAmp.x +
                                    (vec3(specularFactor) - specDielectricF0) * ltcAmp.y) *
                                   ff.y;
                    vec3 areaMetal =
                        (albedoMap * ltcAmp.x + (1.0 - albedoMap) * ltcAmp.y) * ff.y;
                    areaSpec = mix(areaExt, areaMetal, metallicMap);
                }
                vec3 areaDiff =
                    (1.0 - metallicMap) * (1.0 - transmissionEff) * albedoMap * ff.x;

                // One map down the panel normal, multiplied into the whole
                // panel term. The integral it scales is over the rectangle, so
                // this is a hard binary occlusion of a soft source: the panel is
                // either visible from the fragment or it is not, with no partial
                // occlusion of one edge. Aimed at the panel centre, which is the
                // direction the single map was rendered along.
                int aLayer = int(clusterLights[li].shadowMisc.y);
                float aShadow = 1.0;
                if (aLayer >= 0 && alphaMasked == 0) {
                    aShadow = punctualShadow(aLayer, WorldPos, N, normalize(lightPos - WorldPos),
                                             ddxWorld, ddyWorld);
                }

                // Same split as the punctual clamp below: the LTC response is
                // the dimensionless factor, colorIntensity carries the nits.
                Lo += min(areaDiff + areaSpec, vec3(BRDF_MAX)) *
                      clusterLights[li].colorIntensity.xyz * aShadow;

                // Tap the panel's diffuse into the skin buffer, exactly as the
                // punctual site does with its Lambert term. Without this a
                // softbox -- the canonical portrait setup -- produced no
                // subsurface response whatsoever: SSS on and --no-sss rendered
                // byte-identical (spec 11.19, roadmap B3.2).
                //
                // areaDiff is already the diffuse that went into Lo, so the
                // postfx fold hdr + blur(D) - D still cancels against the
                // diffuse actually present. Raw rather than clamp-scaled, which
                // is the punctual tap's convention -- the clamp above bounds a
                // specular spike and the diffuse is not what overshoots.
                //
                // The pre-integrated wrap (skinPreint) stays out: it substitutes
                // a widened falloff for dot(N, L), and a rectangle has no single
                // L. Less of a hole than it sounds -- ff.x already integrates the
                // clamped cosine over the whole rectangle, so a wide panel
                // already wraps geometrically. What is still missing is the
                // widening from subsurface transport, which needs a
                // representative direction and a solid-angle-aware sigma.
                if (sss) {
                    sssDiffuse += areaDiff * clusterLights[li].colorIntensity.xyz * aShadow;
                }
                continue;
            }

            vec3 toLight = lightPos - WorldPos;
            L = normalize(toLight);
            attenuation = getDistanceAtt(dot(toLight, toLight), clusterLights[li].attenCutoff.x);
            attenuation *=
                spotConeFactor(clusterLights[li].dirType.w, clusterLights[li].dirType.xyz,
                                clusterLights[li].attenCutoff.w,
                                clusterLights[li].shadowMisc.x, L);
            // Both terms reach EXACTLY zero over a real area -- the window at the
            // light's range, the cone outside its outer angle -- and cluster
            // assignment is a conservative sphere-vs-AABB, so a boundary cluster
            // hands this loop a band of fragments the light cannot reach. Every
            // term below scales by attenuation, so skipping them costs nothing
            // and saves 9 shadow taps plus the GGX/clearcoat/sheen/POM chain.
            // The old asymptotic falloff was never exactly zero; this is only
            // safe because the photometric one is.
            if (attenuation <= 0.0)
                continue;
            lightCI = clusterLights[li].colorIntensity.xyz;
            lightSize = clusterLights[li].shadowMisc.zw;
            punctualLayer = int(clusterLights[li].shadowMisc.y);
            // A point light owns six layers rather than one; resolve the face
            // here, where its direction is still in scope. -L is the
            // fragment-to-light direction reversed, i.e. light-to-fragment;
            // dominant axis is scale-invariant so the normalize does not matter.
            if (punctualLayer >= 0 && clusterLights[li].dirType.w == 1.0)
                punctualLayer += punctualCubeFace(-L);
        }

        // Half vector, guarded: where the light is directly behind the
        // fragment along the view ray, V + L vanishes and normalize()
        // returns garbage or NaN — and NaN * NdotL(0) stays NaN, painting
        // black flecks around the light's vanishing point. The fallback N
        // is harmless: NdotL is ~0 in that configuration anyway.
        vec3 Hraw = V + L;
        float hLen2 = dot(Hraw, Hraw);
        vec3 H = hLen2 > 1e-8 ? Hraw * inversesqrt(hLen2) : N;
        // Scene radiance, NOT pre-exposed -- Lo is accumulated in scene radiance
        // throughout and converted once at the composite. See the note there.
        vec3 radiance = lightCI * attenuation;

        // Cook-Torrance BRDF with optional anisotropy. The anisotropic branch
        // substitutes only the NDF, so it keeps the shared 1/(4 NdotV NdotL) and
        // the energy compensation below -- which is what makes a per-texel
        // direction a redistribution of the highlight rather than a change to
        // how much light there is.
        float NDF;
        if (aniso > 0.01) {
            NDF = distributionGGXAnisotropic(N, H, anisoT, anisoB, roughnessMap, aniso);
        } else {
            NDF = areaLightNorm * distributionGGX(N, H, areaLightRoughness);
        }
        float G = geometrySmith(N, V, L, roughnessMap);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        // KHR_materials_specular: the spec's dielectric fresnel is
        // mix(f0, f90, (1-|VdotH|)^5) with f90 = specularFactor -- the
        // factor dims GRAZING response too, which folding it into F0 alone
        // cannot. The metallic blend mixes PURE dielectric against PURE
        // metal: Schlick is affine in F0, so blending the results is exact
        // at every metallic, where blending against the pre-mixed standard
        // F would double-count the metal share at partial metalness. Sits
        // before the thin-film branch so iridescence keeps its wholesale
        // replacement.
        vec3 specFdLight = vec3(0.0);
        if (specExt) {
            specFdLight = fresnelSchlickF90(max(dot(H, V), 0.0), specDielectricF0,
                                            vec3(specularFactor));
            F = mix(specFdLight, fresnelSchlick(max(dot(H, V), 0.0), albedoMap), metallicMap);
        }

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
        // KHR_materials_specular: the spec's diffuse trade is the ACHROMATIC
        // 1 - max_value(fresnel), so a colored specular cannot hue-shift the
        // base (per-channel subtraction leaves the tint's complement in the
        // diffuse -- teal under gold). Complemented from the DIELECTRIC
        // fresnel alone; the kD *= 1-metallic below removes the metal share.
        if (specExt) {
            kD = vec3(1.0 - maxComp(specFdLight));
        }
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
            shadow = calculateShadow(dirShadowSlot, fragCascade, WorldPos,
                                     max(lightSize.x, lightSize.y), ddxWorld, ddyWorld);
        }
        // Punctual shadow: this light's own perspective map. Overwrites rather
        // than multiplies because the two are exclusive -- dirShadowSlot is set
        // only on the directional path, punctualLayer only on the cluster one.
        if (punctualLayer >= 0 && alphaMasked == 0) {
            shadow = punctualShadow(punctualLayer, WorldPos, N, L, ddxWorld, ddyWorld);
        }

        // Cloud deck (spec 11.41). Multiplies rather than overwrites: the deck and the
        // cascades occlude the same light independently, and a fragment can be under both.
        // Placed after the punctual branch because that one OVERWRITES.
        //
        // Deliberately NOT gated on alphaMasked, unlike the cascade tap above. That exclusion
        // exists because card-on-card depth compares at map-texel scale are pure acne; a
        // kilometre-scale transmittance field has no such failure, and foliage standing in
        // cloud shadow while the ground beside it does not is exactly the artifact this
        // feature exists to remove.
        shadow *= cloudSunForSlot(WorldPos, dirShadowSlot);

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
        // (velvet / satin) -- no Fresnel, per glTF. Added over the base, which
        // the hoisted sheenScale dims by the lobe's directional albedo.
        vec3 sheenSpec = vec3(0.0);
        if (sheenActive) {
            float Dsh = distributionCharlie(N, H, sheenRough);
            float Vsh = visibilitySheen(NdotL, NdotV, sheenRough);
            sheenSpec = sheenColorPx * Dsh * Vsh;
        }

        // Add this light's contribution with shadow. Firefly clamp: a sub-pixel
        // GGX spike carries far more energy than the pixel legitimately
        // integrates, aliasing into white sparkle across low-roughness
        // normal-mapped surfaces.
        //
        // Clamped on the BRDF, which is where the spike is and which is
        // dimensionless -- NOT on the product, which carries radiance. Bounding
        // the product means bounding a number of NITS, and no fixed number of
        // nits is right at two different light magnitudes or two different
        // exposures. That is what made auto-exposure ratchet (spec 10.1 phase 5).
        vec3 brdf = ((kD * albedoMap / PI + specular) * sheenScale + sheenSpec) *
                        (1.0 - coatAtten) +
                    coatSpec;
        // Scaled by the largest channel's overshoot rather than clamped per
        // channel, so the ratio between channels survives. A per-channel min
        // desaturates a coloured spike toward white -- the same hue-flattening
        // that made the grey cube, one level in and far rarer.
        Lo += brdf * min(1.0, BRDF_MAX / max(maxComp(brdf), 1e-6)) * radiance * NdotL * shadow;

        // SSS: tap this light's Lambert diffuse into the separated skin-diffuse
        // buffer (blurred later; specular stays in FragColor). Separate accumulate
        // -- does not modify the Lo expression above.
        if (sss) {
            sssDiffuse += kD * albedoMap / PI * radiance * NdotL * shadow;
        }

        // Pre-integrated skin: swap this light's clamped-Lambert falloff for the
        // curvature-widened one, as a DELTA so neither statement above has to be
        // rewritten (float reassociation there is the byte-identity gate for
        // every non-skin material).
        //
        // It goes into Lo AND the tap because postfx folds this back as
        // hdr + blur(D) - D, and that subtraction only cancels when D is the
        // diffuse actually present in hdr; change one and the response arrives
        // as a signed second difference of itself, which measures at 5% of the
        // intended effect. The dark side is where the difference lives, so
        // dot(N, L) is recomputed UNCLAMPED -- NdotL above is clamped at 0.
        //
        // Each side is scaled by the factors ITS OWN diffuse already carries:
        // Lo's by the sheen dimming, the clearcoat attenuation and the firefly
        // clamp, the tap by none of them. That makes the substitution
        // NdotL -> D on each side separately. Adding the same raw term to both
        // instead leaves Lo's diffuse at Lu*(D - (1-c)*NdotL), which turns
        // NEGATIVE once c falls below ~0.58 at the widest sigma -- and the
        // firefly clamp alone reaches 0.39 at the roughness floor, so a gloss
        // highlight on clearcoated skin would push negative radiance into an
        // RGBA16F target that feeds the bloom brightpass and the TAA history.
        if (skinPreint) {
            vec3 dSkin = kD * albedoMap / PI *
                         (skinDiffuse(skinShapeF, dot(N, L)) - vec3(NdotL)) * radiance * shadow;
            Lo += dSkin * sheenScale * (1.0 - coatAtten) *
                  min(1.0, BRDF_MAX / max(maxComp(brdf), 1e-6));
            // Guarded: with SSS off attachment 4 is unbound and the accumulator
            // is discarded, which is exactly the --no-sss configuration the
            // curvature gate measures in.
            if (sss)
                sssDiffuse += dSkin;
        }

        // SSS back-light transmission: thin-region glow when the light is
        // behind the surface. Guarded so non-skin / --no-sss is byte-identical.
        if (sss) {
            Lo += subsurfaceTransmission(N, L, V, albedoMap, subsurfaceColor, subsurface,
                                         lightCI * attenuation);
        }
    }

    // Ambient lighting with IBL
    // Ambient. The DIFFUSE source and the SPECULAR lobe are chosen
    // independently: diffuse comes from the probe volume when one has converged
    // and from the environment's irradiance map otherwise, while a specular lobe
    // exists only when there is an environment to reflect. Writing that as one
    // branch per combination is what previously produced a third arm that
    // re-derived this same Lambert term and a second SSS tap kept in step by
    // hand -- with a comment whose whole job was to warn about the drift.
    //
    // Two accumulators: `ambient` and `ambSpec`. Under splitAmbientSpec the
    // env-specular share -- the base lobe plus the clearcoat and sheen lobes,
    // each through the same layer dims as the inline sum -- collects in
    // ambSpec and routes to SpecOut, where the post chain occludes exactly it.
    // The inline branches keep the original expressions verbatim: they are the
    // byte-identity gate for every non-split mode.
    vec3 ambient;
    vec3 ambSpec = vec3(0.0);
    if (iblEnabled > 0 || giEnabled > 0) {
        // kS is the share the ambient SPECULAR lobe takes. No environment means
        // no such lobe, so nothing is taken and all the non-metal energy stays
        // diffuse. That single substitution is the entire difference between an
        // environment-lit surface and a probe-lit one.
        vec3 F = iblEnabled > 0 ? fresnelSchlickRoughness(NdotV, F0, roughnessMap) : vec3(0.0);
        // KHR_materials_specular, ambient side: the factor COMPOSES with the
        // engine's roughness-grazing f90 convention (max(1-r, f0) * factor)
        // rather than replacing it -- at factor 1 and white color this
        // reduces exactly to the default path, the extension's no-op
        // invariant. Pure dielectric vs pure metal, per the analytic site.
        vec3 specFdAmbient = vec3(0.0);
        if (specExt && iblEnabled > 0) {
            vec3 f90a =
                max(vec3(1.0 - roughnessMap), specDielectricF0) * specularFactor;
            specFdAmbient = fresnelSchlickF90(NdotV, specDielectricF0, f90a);
            F = mix(specFdAmbient, fresnelSchlickRoughness(NdotV, albedoMap, roughnessMap),
                    metallicMap);
        }

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        // Bare specExt: with no IBL, F is vec3(0) and both forms agree, so
        // the narrower gate above needs no mirror here.
        if (specExt) {
            kD = vec3(1.0 - maxComp(specFdAmbient));
        }
        kD *= 1.0 - metallicMap;
        kD *= 1.0 - transmissionEff; // diffuse yields to the transmitted term

        // Both sources store the same quantity -- the cosine-weighted mean
        // incident radiance, E/pi -- so this is a straight substitution and the
        // albedo multiply below is unchanged.
        vec3 irradiance = giEnabled > 0 ? giSampleIrradiance(WorldPos, N, V)
                                        : texture(irradianceMap, N).rgb;
        vec3 diffuse = irradiance * albedoMap;

        // The environment's strength knob, and 1.0 when there is no environment
        // for it to describe. Note it currently scales probe irradiance too,
        // which is arguably double-counting -- the captures the volume
        // integrates were already lit by the environment. Left as-is here
        // deliberately: correcting it moves pixels on the GI-plus-IBL path,
        // which no golden covers, so it wants its own change and its own gate.
        float envScale = iblEnabled > 0 ? iblIntensity : 1.0;

        // Shared by the specular lobe below and by the sheen lobe further down,
        // so it outlives the iblEnabled block both sit inside.
        vec3 R = reflect(-V, N);

        vec3 specular = vec3(0.0);
        if (iblEnabled > 0) {
        vec3 prefilteredColor;
        if (probeEnabled > 0) {
            // prefilteredMap holds the PROBE's prefilter here, bound over the
            // global environment's unit. Both carry absolute radiance, which is
            // what lets them share this path and the composite's single
            // conversion of `ambient` -- see render.c's capture-at-unity note.
            //
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
        // Reuses the brdf fetched before the light loop (same coordinates).
        // brdf.y is the split-sum's f90 = 1 lobe; KHR_materials_specular
        // scales f90 by the factor, so the extension path dims that term --
        // the "IBL specular weight-dimming" 4.10.1 deferred. Metals keep
        // the full term (dielectric-only extension). The if/else DUPLICATION
        // is load-bearing: a shared `* iblF90` on the default line is the
        // direct-weight formulation 4.10.1 tried and reverted (compiler
        // reassociation broke the cmp gate). energyComp needs no change:
        // Ess is the f90 = 1 single-scatter albedo and the factor-scaled
        // lobe passes through it linearly.
        if (specExt) {
            float iblF90 = mix(specularFactor, 1.0, metallicMap);
            specular = prefilteredColor * (F * brdf.x + iblF90 * brdf.y) * energyComp;
        } else {
            specular = prefilteredColor * (F * brdf.x + brdf.y) * energyComp;
        }
        }

        if (splitAmbientSpec > 0) {
            // aoMap dims the DIFFUSE share only: baked AO models diffuse
            // hemispherical occlusion, and the specular lobe's occlusion is
            // the post chain's directional cone term. The inline arm below
            // deliberately keeps aoMap on the sum.
            ambient = kD * diffuse * aoMap * envScale;
            ambSpec = specular * envScale;
        } else {
            ambient = (kD * diffuse + specular) * aoMap * envScale;
        }

        // SSS: tap the ambient Lambert diffuse into the skin-diffuse buffer too.
        // ONE tap, reusing the exact sub-expression above -- there is no second
        // path for it to drift from.
        if (sss) {
            sssDiffuse += kD * diffuse * aoMap * envScale;
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
            // The coat dims BOTH shares (it sits over the whole surface); its
            // own lobe is specular, so it joins ambSpec when splitting.
            if (splitAmbientSpec > 0) {
                ambient = ambient * (1.0 - ccF);
                ambSpec = ambSpec * (1.0 - ccF) + coatIBL * iblIntensity;
            } else {
                ambient = ambient * (1.0 - ccF) + coatIBL * aoMap * iblIntensity;
            }
        }

        // Sheen IBL as a split-sum: the Charlie-prefiltered env at the sheen
        // roughness times the LUT's directional albedo E. The base ambient is
        // dimmed by the same hoisted sheenScale as the analytic path -- one
        // layer-over-base energy convention. Guarded so the base ambient
        // lines above stay byte-identical when sheen is off.
        if (sheenActive) {
            vec3 sheenPre = textureLod(charliePrefilteredMap, R, sheenRough * maxCharlieLOD).rgb;
            // Sheen dims BOTH shares (same layer-over-base convention as the
            // coat); the sheen lobe itself is specular.
            if (splitAmbientSpec > 0) {
                ambient = ambient * sheenScale;
                ambSpec = ambSpec * sheenScale +
                          sheenColorPx * sheenPre * sheenE * iblIntensity;
            } else {
                ambient = ambient * sheenScale +
                          sheenColorPx * sheenPre * sheenE * aoMap * iblIntensity;
            }
        }
    } else {
        // No IBL: a uniform ambient the scene authors, in cd/m^2 like every other
        // emitter. Diffuse-only, so it yields to transmission like the other
        // diffuse terms. Zero by default -- an unlit surface is black, because
        // that is what an unlit surface is.
        //
        // This replaced a hardcoded "3% of white", which had no correct form. As
        // scene radiance it was a fixed quantity tied to no light, so scaling
        // every light broke it (the scale-invariance failure). Made
        // display-relative to fix that, it became a term whose ABSOLUTE value
        // grew as exposure closed, and auto-exposure -- which meters absolute
        // radiance -- chased it (spec 10.1 phase 5). No constant satisfies both,
        // because the term was never physical. A real emitter is.
        ambient = ambientRadiance * albedoMap * aoMap * (1.0 - transmissionEff);
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
        // KHR_materials_volume absorption, over the same path the bend above
        // travelled. attenuationColor is defined as what survives exactly
        // attenuationDistance, so the extinction reproducing it is
        // -log(color)/distance. Distance 0 is this material's spelling of the
        // extension's infinite default and leaves the sample untouched.
        //
        // The colour is clamped off zero because a fully absorbing channel is an
        // infinite extinction, and inf * 0 -- an opaque tint on thin glass, which
        // is a legal authoring combination -- is NaN, not black.
        if (attenuationDistance > 0.0) {
            vec3 sigmaT = -log(max(attenuationColor, vec3(1e-5))) / attenuationDistance;
            sceneSample *= exp(-sigmaT * transmissionThickness);
        }
        vec3 Ft = fresnelSchlickRoughness(NdotV, F0, roughnessMap);
        // KHR_materials_specular: the transmitted share is an energy trade
        // against the specular layer, so the extension path uses the same
        // composed f90 and achromatic complement as kD. Dielectric fresnel
        // alone -- the (1 - metallicMap) factor below removes the metal
        // share, mirroring kD's structure.
        if (specExt) {
            vec3 f90t =
                max(vec3(1.0 - roughnessMap), specDielectricF0) * specularFactor;
            Ft = vec3(maxComp(fresnelSchlickF90(NdotV, specDielectricF0, f90t)));
        }
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
    // THE conversion to working space, for every term, exactly once.
    //
    // It used to sit on the per-light `radiance` instead, because a clamp
    // measured in nits sat between there and here and had to see converted
    // values. Spec 10.2 phase 5 made that clamp dimensionless (BRDF_MAX), so
    // nothing in between is scale-sensitive any more and preExposure factors
    // straight out of the whole sum.
    //
    // Which matters because Lo is accumulated from four places -- the punctual
    // loop, the area-light branch, the SSS back-transmission, and emissive --
    // and when each needed its own multiply, three of them did not get one.
    // Area lights were off by 1/preExposure at every exposure but 1.0, which no
    // golden could see. One conversion site cannot have that bug.
    //
    // The fp16 ceiling then guards a working-space value, which is the only
    // scale at which a fixed ceiling means anything. RGBA16F maxes at 65504 and
    // a tight GGX spike (peak ~1.2e5 at the 0.04 roughness floor, times grazing
    // Fresnel) would otherwise store +INF, which the tonemap turns to NaN and
    // shows as black flecks tracing the highlights.
    vec3 color =
        min((Lo + ambient + transmitted + emissiveMap) * preExposure, vec3(WS_SCENE_MAX));

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
        // clusterCount is the list this fragment actually shaded with
        if (clusterCount > 0) {
            float t = clamp(float(clusterCount) / 16.0, 0.0, 1.0);
            vec3 ramp = t < 0.25   ? mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), t * 4.0)
                        : t < 0.5  ? mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), (t - 0.25) * 4.0)
                        : t < 0.75 ? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), (t - 0.5) * 4.0)
                                   : mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), (t - 0.75) * 4.0);
            color = mix(color, ramp, 0.6);
        }
    }



    /*
     * HDR hotspots: what the SCENE pass produced, banded, before the post chain runs.
     *
     * Debug modes take a passthrough blit, so nothing downstream can add a speck here or hide
     * one. A fragment that already reads red was shaded that bright; a speck visible only in
     * PBR was made after this point. Bands rather than a gradient because the question is
     * which STOP a pixel is on -- an eye cannot rank two dim greys, and 4x versus 16x mid grey
     * is the difference between a hot highlight and a broken one.
     *
     * Pre-exposure is divided out so the ramp means the same thing whatever the metering is
     * doing; otherwise every band moves when the exposure drifts and nothing can be compared
     * between two frames.
     */
    if (renderMode == 10 || renderMode == 11) {
        /*
         * Banded on the PRE-EXPOSED value, which is the number the bloom threshold and the
         * tonemap actually see -- "will this speck bloom" is a question about this scale, not
         * about absolute radiance. Dividing pre-exposure back out answered a different
         * question and, in a scene lit by a 0.8 degree sun, put every pixel in one band.
         *
         * Nine stops rather than five, because the interesting comparison is between a
         * subject and a speck ON that subject, and in a dark frame both live near the bottom.
         * Each band is 4x the one below it.
         */
        /*
         * Mode 10 bands the shaded colour; mode 11 bands the SSS DIFFUSE instead, which is the
         * buffer the scatter pyramid is built from. They are different numbers: attachment 4
         * carries subsurface * sssDiffuse, so it can be hot where FragColor is not, and a
         * pyramid amplifies whatever it was given. Banding only the visible colour cannot see
         * a bad input to a later pass.
         */
        vec3 probe = renderMode == 11 ? subsurface * sssDiffuse * preExposure : color;
        float lum = dot(probe, vec3(0.2126, 0.7152, 0.0722));
        vec3 band = lum > 64.0      ? vec3(1.0, 1.0, 1.0)    // far past anything displayable
                    : lum > 16.0    ? vec3(1.0, 0.0, 0.0)    // blown
                    : lum > 4.0     ? vec3(1.0, 0.45, 0.0)   // very hot
                    : lum > 1.0     ? vec3(1.0, 1.0, 0.0)    // at or above white
                    : lum > 0.25    ? vec3(0.4, 0.9, 0.2)    // brightly lit
                    : lum > 0.0625  ? vec3(0.0, 0.55, 0.1)   // ordinary lit
                    : lum > 0.0156  ? vec3(0.0, 0.35, 0.5)   // dim
                    : lum > 0.0039  ? vec3(0.1, 0.1, 0.5)    // very dim
                                    : vec3(0.02, 0.02, 0.08); // effectively black
        FragColor = vec4(band, finalOpacity);
        NormalOut = vec4(0.0);
        VelocityOut = packVelocityAux(ViewPos.z, roughnessMap);
        AlbedoOut = vec4(0.0);
        DiffuseOut = vec4(0.0);
        SpecOut = vec4(0.0, 0.0, 0.0, finalOpacity);
        return;
    }

    // `color` is already working space -- converted upstream so the clamps in
    // between operate on the scale they were written for.
    FragColor = vec4(color, finalOpacity);

    // G-buffer: view-space normal (xyz) for SSAO; alpha is a non-negative
    // reflective marker — only the shadow catcher's negative alpha traces in
    // SSR, so model surfaces stamp 0 (never reflected in screen space; they
    // rely on IBL). Alpha-to-coverage surfaces (hair) stamp zero normals
    // instead: consumers must not trust normals at strand scale, and leaving
    // the buffer unwritten is worse (stale normals of whatever drew behind
    // the hair, under the hair's depth). Their alpha must mirror FragColor's:
    // Apple's driver derives A2C coverage from the last color output's alpha,
    // not output 0, and any other value reshapes the card cutouts. Measured --
    // forcing this alpha to 1.0 moves 8580 px of masked hair.
    //
    // "Last" means last output carrying an alpha CHANNEL: attachment 7 sits
    // above this one and its alpha is inert (see SpecOut). The hazard that
    // leaves is attachment 4, which does have alpha and holds an SSS profile
    // index -- if it ever binds alongside masked geometry it would take the
    // coverage this write is providing. No scene has both today, so it is
    // predicted rather than observed, and confirming it needs a fixture
    // carrying subsurface and alpha-mask at once.
    NormalOut = alphaMasked > 0
                    ? vec4(0.0, 0.0, 0.0, finalOpacity)
                    : vec4(normalize(mat3(view) * N), 0.0);

    // Auxiliary G-buffer: screen-space motion vector (.xy, un-jittered current
    // vs previous, UV units) for TAA reprojection, linear view-space Z (.z) for
    // GTAO position reconstruction, and effective perceptual roughness (.w) for
    // the tonemap specular-occlusion pass. Reconstructing occlusion positions
    // from a linear Z avoids the non-linear DEPTH24 buffer, whose grazing-angle
    // quantization staircases the reconstructed floor into AO banding.
    VelocityOut = packVelocityAux(ViewPos.z, roughnessMap);
    AlbedoOut = vec4(albedoMap, metallicMap);
    // SSS diffuse: subsurface-scaled skin diffuse (0 off-skin) in .rgb, this
    // material's scatter-profile index + 1 in .a (0 = non-skin, so the blur
    // rejects taps that cross into another material or the background). The SSS
    // post pass blurs .rgb and composites hdr + blur - this (diffuse softens,
    // FragColor's specular stays sharp), reading .a per pixel to select the
    // profile. Discarded unless the engine enables attachment 4 (sssEnabled).
    // Converted here for the same reason FragColor is, and it has to be the same
    // conversion: postfx folds this back as hdr + blur(D) - D, so if D is on a
    // different scale the subtract does not cancel the sharp diffuse already in
    // hdr and skin goes negative.
    //
    // sssDiffuse is filled from two taps -- the direct one in the light loop and
    // the IBL one below it. Both are scene radiance now; while the loop
    // pre-exposed and the IBL tap did not, this attachment held a mix of two
    // scales and the comment here asserted it did not.
    //
    // Clamped like FragColor: this is an RGBA16F attachment too, and it was the
    // one G-buffer write with no ceiling at all.
    DiffuseOut = vec4(min(subsurface * sssDiffuse * preExposure, vec3(WS_SCENE_MAX)),
                      float(max(sssProfileIndex + 1, 0)));

    // Ambient specular, same working-space conversion and fp ceiling as
    // FragColor (the composite adds this back into the same buffer, so it must
    // be on the same scale). Zero on the inline path -- the attachment is only
    // bound when splitting, but every bound attachment must be written.
    SpecOut = vec4(min(ambSpec * preExposure, vec3(WS_SCENE_MAX)), finalOpacity);

    // The OIT accumulate (guarded so passMode 0 leaves FragColor and the opaque/
    // alpha-blend paths byte-identical): premultiplied colour times how much of
    // this fragment survives what is in front of it -- measured from the moments
    // where they were generated, guessed from the depth curve otherwise. Alpha
    // into RevealageOut, whose multiplicative blend builds the product of (1-a).
    if (passMode == 1) {
        float w = oitMomentWeighted > 0 ? mboitTransmittanceAtlas(oitMomentTex, oitMomentInvSize,
                                                                 oitNearFar, -ViewPos.z)
                                        : oitWeight(-ViewPos.z);
        // Clamp the premultiplied weighted color to the fp16 ceiling (the accum
        // target is RGBA16F) so a bright near-camera translucent frag -- where the
        // depth weight nears its max -- can't write Inf and resolve to a white blob.
        AccumOut = vec4(min(color * finalOpacity * w, vec3(65504.0)), finalOpacity * w);
        RevealageOut = vec4(finalOpacity);
    }
}
