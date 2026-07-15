#version 330 core
in vec3 WorldPos;
layout(location = 0) out vec4 FragColor;
// G-buffer for screen-space passes: view-space normal (xyz) + roughness (a).
// Only lands when the engine enables color attachment 1; otherwise discarded.
layout(location = 1) out vec4 NormalOut;

// Shadow catcher: an invisible ground plane that only darkens where the
// shadow maps say the shadow-casting lights are occluded. Drawn after the
// skybox with alpha blending so it grounds the model on the projected
// environment floor. Each light's shadow is weighted by that light's share
// of the total analytic light so secondary lights cast fainter shadows.
#define MAX_SHADOW_LIGHTS 3

uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS];
uniform float shadowLightWeight[MAX_SHADOW_LIGHTS];
uniform int numShadowLights;
uniform vec2 shadowTexelSize;
uniform float shadowBias;
uniform float catcherStrength;
uniform float planeRadius;
uniform mat4 view;
// SSR hook (dormant until reflections land): surfaceMode 1 skips the
// unshadowed-fragment discard so the whole floor writes depth and normals
// (color blends at alpha 0, leaving the image untouched)
uniform int surfaceMode;
uniform float floorRoughness;

float occlusion_from(int slot)
{
    vec4 lightSpace = lightSpaceMatrix[slot] * vec4(WorldPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;

    // 5x5 PCF with a widened kernel for soft edges
    float shadow = 0.0;
    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize * 1.5;
            float depth = texture(shadowMaps, vec3(proj.xy + offset, float(slot))).r;
            shadow += proj.z - shadowBias > depth ? 1.0 : 0.0;
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
    NormalOut = vec4(normalize(mat3(view) * vec3(0.0, 1.0, 0.0)), floorRoughness);
}
