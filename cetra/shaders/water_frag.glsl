#version 330 core

/*
 * Water surface shading (spec 11.32).
 *
 * A single-layer water interface: exact dielectric Fresnel splits the view into
 * a reflected share taken from the environment cubemap and a transmitted share
 * taken from the resolved opaque scene, attenuated through the body by
 * Beer-Lambert over the real path length the depth buffer gives.
 *
 * Output is LINEAR WORKING SPACE, like every other pass that writes the scene
 * HDR buffer. Anything sampled in absolute scene radiance -- the environment
 * cubemap, the authored scatter colour -- is multiplied by preExposure here; the
 * resolved scene colour is NOT, because it was written pre-exposed and would be
 * counted twice. view.glsl is the authority on that contract.
 */

// The full G-buffer. Every location the opaque draw-buffer list can enable has
// to be written: engine_set_scene_draw_buffers turns a slot on when a CONSUMER
// wants it that frame, not per draw, so a location left unwritten while enabled
// leaves whatever the pass before put there.
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalOut;
layout(location = 2) out vec4 VelocityOut;
layout(location = 3) out vec4 AlbedoOut;
layout(location = 4) out vec4 DiffuseOut;
layout(location = 7) out vec4 SpecOut;

in vec3 WorldPos;
in vec3 ViewPos;
in vec3 Normal;
in vec4 CurrClip;
in vec4 PrevClip;

uniform mat4 view;
uniform mat4 projection;
uniform vec2 screenSize;

uniform float waterRoughness;
uniform float waterIor;
uniform vec3 waterAbsorption; // extinction per world unit, per channel
uniform vec3 waterScatter;    // in-scattered colour, absolute scene radiance

// Mipped resolve of the opaque scene (colour + skybox), already pre-exposed.
uniform sampler2D sceneColorTex;
uniform int sceneColorAvailable;
// Single-sample resolved scene depth. This is the tap pbr_frag has no sampler
// slot left for, and the reason water ships its own program.
uniform sampler2D sceneDepthTex;
uniform int sceneDepthAvailable;

// Split-sum environment, bound by bind_ibl_textures. The procedural sky bakes
// into this cubemap, so the reflection follows the sun with no extra plumbing.
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;

#include "view.glsl"
#include "depth.glsl"

// Coarsest mip the transmission may select. The resolve stops generating there.
const float WATER_TRANSMISSION_MAX_LOD = 6.0;
// Absorption is evaluated over a clamped path: past a few extinction lengths the
// exponential is already zero, and an unbounded value from a sky-depth sample
// would only cost precision.
const float WATER_MAX_PATH = 64.0;

vec2 screenVelocity() {
    return (CurrClip.xy / CurrClip.w - PrevClip.xy / PrevClip.w) * 0.5;
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec2 uv = gl_FragCoord.xy / max(screenSize, vec2(1.0));

    vec3 N = normalize(Normal);
    vec3 V = normalize(-ViewPos);
    // The interface is shaded in view space, so the normal has to arrive there
    // too -- the surface normal is authored in world space by ocean.glsl.
    vec3 Nv = normalize(mat3(view) * N);
    float NdotV = clamp(dot(Nv, V), 0.0, 1.0);

    // Optical path through the body: the view-Z gap between this surface and
    // whatever the depth buffer holds behind it, stretched from planar depth onto
    // the actual view ray. Without the stretch a grazing sight line would be
    // charged the vertical column and read too shallow exactly where water is
    // deepest-looking.
    float surfaceDist = max(-ViewPos.z, 1e-4);
    float path = WATER_MAX_PATH;
    if (sceneDepthAvailable == 1) {
        float bedNdc = texture(sceneDepthTex, uv).r * 2.0 - 1.0;
        float bedDist = -viewZFromNdcZ(bedNdc);
        float rayScale = length(ViewPos) / surfaceDist;
        path = max(bedDist - surfaceDist, 0.0) * rayScale;
    }
    path = min(path, WATER_MAX_PATH);

    // Transmitted share: the scene behind, bent along the refracted ray by the
    // path it travels, then absorbed over that same path.
    vec3 refrDir = refract(-V, Nv, 1.0 / waterIor);
    vec3 bed;
    if (sceneColorAvailable == 1) {
        vec3 exitView = ViewPos + refrDir * min(path, 8.0);
        vec4 refrClip = projection * vec4(exitView, 1.0);
        vec2 refrUV = clamp(refrClip.xy / refrClip.w * 0.5 + 0.5, vec2(0.001), vec2(0.999));
        bed = textureLod(sceneColorTex, refrUV, waterRoughness * WATER_TRANSMISSION_MAX_LOD).rgb;
    } else {
        bed = vec3(0.0);
    }
    vec3 T = exp(-waterAbsorption * path);
    vec3 body = bed * T + waterScatter * preExposure * (1.0 - T);

    // Reflected share: the split-sum environment lobe, the same lookup every
    // other material makes. F0 0.020 falls out of IOR 1.333 rather than being
    // authored.
    float f0s = (waterIor - 1.0) / (waterIor + 1.0);
    vec3 F0 = vec3(f0s * f0s);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, waterRoughness);
    vec2 ab = texture(brdfLUT, vec2(NdotV, waterRoughness)).rg;
    vec3 specWeight = F * ab.x + ab.y;
    vec3 reflected = vec3(0.0);
    if (iblEnabled > 0) {
        vec3 R = reflect(-V, Nv);
        vec3 Rw = normalize(mat3(transpose(view)) * R);
        reflected = textureLod(prefilteredMap, Rw, waterRoughness * maxReflectionLOD).rgb *
                    iblIntensity * preExposure;
    }

    // Energy split: what the interface reflects it does not transmit.
    vec3 color = body * (1.0 - specWeight) + reflected * specWeight;

    FragColor = vec4(min(color, vec3(WS_SCENE_MAX)), 1.0);
    // Alpha 0: an opaque model surface, NOT the catcher's negative reflective
    // marker. SSR only shades the catcher, and giving water the marker would put
    // it in a channel whose magnitude is already the catcher's edge falloff --
    // the two are not separable there (see the spec's phase 1b note).
    NormalOut = vec4(Nv, 0.0);
    // Linear view-Z in .z is what makes the atmosphere composite fog this
    // surface at the water's own depth rather than the bed's.
    VelocityOut = vec4(screenVelocity(), ViewPos.z, waterRoughness);
    AlbedoOut = vec4(waterScatter, 1.0);
    DiffuseOut = vec4(0.0);
    // No ambient specular routed out for occlusion: water's reflection stays in
    // FragColor above, so there is nothing here for the spec-occ composite to
    // scale and nothing double-counted by leaving it at zero.
    SpecOut = vec4(0.0);
}
