#version 330 core
in vec3 WorldPos;
layout(location = 0) out vec4 FragColor;
// G-buffer for SSR: view-space up-normal .xyz + the reflective marker .a (see
// the write below). Only lands when the engine enables color attachment 1.
layout(location = 1) out vec4 NormalOut;
// When SSR enables the G-buffer around this draw, EVERY attachment in the
// draw-buffer list must be written -- an unwritten one is undefined, and
// stray garbage in the aux buffer's linear-Z paints nondeterministic GTAO/GI
// artifacts across the floor. Zero is the deliberate value for both: aux 0 is
// the "sky" sentinel (screen-space AO/GI skip the projected floor; velocity 0
// suits a static plane) and albedo 0 means no bounce tint.
layout(location = 2) out vec4 AuxOut;
layout(location = 3) out vec4 AlbedoOut;
layout(location = 4) out vec4 DiffuseOut; // SSS skin-diffuse; the floor is never skin

// Shadow catcher: an invisible ground plane that only darkens where the
// shadow maps say the shadow-casting lights are occluded. Drawn after the
// skybox with alpha blending so it grounds the model on the projected
// environment floor. Each light's shadow is weighted by that light's share
// of the total analytic light so secondary lights cast fainter shadows.
// 5x5 PCF: the catcher is a soft scene-scale grounding shadow, so it takes a
// wider kernel than the particle motes do.
#define CSM_OUTERMOST_PCF
#define CSM_PCF_HALF_KERNEL 2
#include "csm.glsl"

// Catcher-only: each light's shadow is weighted by that light's share of the
// total analytic light, so secondary lights cast fainter shadows.
uniform float shadowLightWeight[MAX_SHADOW_LIGHTS];
uniform float catcherStrength;
uniform float planeRadius;
uniform mat4 view;
// surfaceMode 1 (set only when SSR is active) skips the unshadowed-fragment
// discard so the whole floor writes depth and the reflective marker for the
// reflection march; color still blends at alpha 0, leaving the image
// untouched.
uniform int surfaceMode;

float occlusion_from(int slot)
{
    return csmOutermostOcclusion(WorldPos, slot);
}

void main()
{
    float darkness = 0.0;
    for (int i = 0; i < numShadowLights && i < MAX_SHADOW_LIGHTS; i++) {
        darkness += shadowLightWeight[i] * occlusion_from(i);
    }

    // Fade out toward the plane edge so the quad boundary is invisible
    float dist = length(WorldPos.xz);
    float falloff = 1.0 - smoothstep(0.4 * planeRadius, 0.9 * planeRadius, dist);

    float alpha = darkness * catcherStrength * falloff;
    if (alpha < 0.005 && surfaceMode == 0)
        discard;

    FragColor = vec4(0.0, 0.0, 0.0, alpha);
    // Negative alpha is the "reflective floor" marker the SSR march traces;
    // its magnitude is the reflectivity weight, carrying the same edge
    // falloff as the shadow so reflections dissolve at the quad boundary the
    // way the shadow does (the env-fallback reflection would otherwise print
    // the catcher's finite rectangle into the scene). Only stamped when SSR
    // is active — otherwise a non-negative marker keeps the floor out of the
    // reflection set, and SSAO on this texel keeps using its derivative
    // normal rather than this up-normal.
    NormalOut =
        vec4(normalize(mat3(view) * vec3(0.0, 1.0, 0.0)), surfaceMode == 1 ? -falloff : 0.0);
    AuxOut = vec4(0.0);
    AlbedoOut = vec4(0.0);
    DiffuseOut = vec4(0.0);
}
