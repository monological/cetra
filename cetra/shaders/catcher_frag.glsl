#version 330 core
in vec3 WorldPos;
layout(location = 0) out vec4 FragColor;
// G-buffer for SSR: view-space up-normal + the floor's roughness. Only
// lands when the engine enables color attachment 1 around this draw.
layout(location = 1) out vec4 NormalOut;
// When SSR enables the G-buffer around this draw, EVERY attachment in the
// draw-buffer list must be written -- an unwritten one is undefined, and
// stray garbage in the aux buffer's linear-Z paints nondeterministic GTAO/GI
// artifacts across the floor. Zero is the deliberate value for both: aux 0 is
// the "sky" sentinel (screen-space AO/GI skip the projected floor; velocity 0
// suits a static plane) and albedo 0 means no bounce tint.
layout(location = 2) out vec4 AuxOut;
layout(location = 3) out vec4 AlbedoOut;

// Shadow catcher: an invisible ground plane that only darkens where the
// shadow maps say the shadow-casting lights are occluded. Drawn after the
// skybox with alpha blending so it grounds the model on the projected
// environment floor. Each light's shadow is weighted by that light's share
// of the total analytic light so secondary lights cast fainter shadows.
#define MAX_SHADOW_LIGHTS 3
#define SHADOW_CASCADES 3

uniform sampler2DArray shadowMaps;
// Layers stride by the RUNTIME cascadeCount (layer = slot*cascadeCount + c),
// so at cascadeCount 1 the indices match the classic single-map layout
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
uniform vec4 cascadeParams[MAX_SHADOW_LIGHTS * SHADOW_CASCADES]; // width, near, far, biasScale
uniform int cascadeCount;
uniform float shadowLightWeight[MAX_SHADOW_LIGHTS];
uniform int numShadowLights;
uniform vec2 shadowTexelSize;
uniform float shadowBias;
// World width of the scene-fit (single-cascade) ortho map: the reference
// the PCF kernel's world size was tuned against
uniform float sceneOrthoWidth;
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
    // The catcher is a soft scene-scale grounding shadow: it samples the
    // OUTERMOST cascade, which is the classic camera-independent scene-fit
    // map (complete for every caster in the scene by construction). One map
    // means no per-fragment selection, no seams, and no boundary that can
    // move with the camera -- exactly the pre-cascade floor behavior. At
    // cascadeCount 1 the layer is the classic slot index.
    int layer = slot * cascadeCount + (cascadeCount - 1);
    vec4 lightSpace = lightSpaceMatrix[layer] * vec4(WorldPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;

    // params.w and the kernel ratio are exactly 1.0 for the scene-fit map;
    // kept so the expressions stay uniform with the pbr consumer
    float bias = shadowBias * cascadeParams[layer].w;

    // 5x5 PCF with a widened kernel for soft edges
    vec2 kernelStep = shadowTexelSize * 1.5 * (sceneOrthoWidth / cascadeParams[layer].x);
    float shadow = 0.0;
    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * kernelStep;
            float depth = texture(shadowMaps, vec3(proj.xy + offset, float(layer))).r;
            shadow += proj.z - bias > depth ? 1.0 : 0.0;
        }
    }
    return shadow / 25.0;
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
    // its magnitude is unused (floor roughness is a scalar uniform on the
    // SSR pass). Only stamped when SSR is active — otherwise a non-negative
    // marker keeps the floor out of the reflection set, and SSAO on this
    // texel keeps using its derivative normal rather than this up-normal.
    NormalOut = vec4(normalize(mat3(view) * vec3(0.0, 1.0, 0.0)), surfaceMode == 1 ? -1.0 : 0.0);
    AuxOut = vec4(0.0);
    AlbedoOut = vec4(0.0);
}
