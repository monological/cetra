#version 330 core

// M3 particle shading: soft HDR sprite lit by the directional key and dimmed
// inside the scene's CSM shadow, so motes brighten in the light and fall dark
// in shadow. Output is premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
// See specs/5.0-particle-system.md.

in vec2 vCorner;
in vec4 vColor;
in float vLifeFrac;
in vec3 vWorldPos;

layout(location = 0) out vec4 FragColor;

uniform float hdrGain;  // push color above 1.0 so bloom catches the motes
uniform vec3 uSunColor; // key light color (subtle warm tint where it hits)
uniform float uAmbient; // brightness floor for shadowed motes

// CSM subset filled by bind_shadow_maps_to_program (location-guarded on the C
// side, so declaring only what we sample is fine). occlusion_from samples the
// outermost scene-fit cascade -- no per-fragment cascade selection, no seams --
// which is exactly right for volumetric motes (mirrors catcher_frag.glsl).
#define MAX_SHADOW_LIGHTS 3
#define SHADOW_CASCADES 3
uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
uniform vec4 cascadeParams[MAX_SHADOW_LIGHTS * SHADOW_CASCADES]; // width, near, far, biasScale
uniform int cascadeCount;
uniform vec2 shadowTexelSize;
uniform float shadowBias;
uniform float sceneOrthoWidth;
uniform int numShadowLights;

float occlusion_from(int slot) {
    int layer = slot * cascadeCount + (cascadeCount - 1);
    vec4 lightSpace = lightSpaceMatrix[layer] * vec4(vWorldPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;

    float bias = shadowBias * cascadeParams[layer].w;
    vec2 kernelStep = shadowTexelSize * 1.5 * (sceneOrthoWidth / cascadeParams[layer].x);
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            vec2 offset = vec2(float(x), float(y)) * kernelStep;
            float depth = texture(shadowMaps, vec3(proj.xy + offset, float(layer))).r;
            shadow += proj.z - bias > depth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

void main() {
    // Soft round sprite from the radial distance across the quad.
    float r = length(vCorner);
    float a = smoothstep(1.0, 0.35, r) * vColor.a;

    // Life alpha envelope: fade in over the first 10%, out over the last 30%.
    float fadeIn = smoothstep(0.0, 0.1, vLifeFrac);
    float fadeOut = 1.0 - smoothstep(0.7, 1.0, vLifeFrac);
    a *= fadeIn * fadeOut;

    // Motes brighten in the key, fall to the ambient floor in shadow. Billboards
    // have no meaningful normal, so this is a light-vs-shadow term, not N.L.
    float occ = (numShadowLights > 0) ? occlusion_from(0) : 0.0;
    float sun = 1.0 - occ;
    float bright = uAmbient + (1.0 - uAmbient) * sun;
    vec3 tint = mix(vec3(1.0), uSunColor, 0.4 * sun); // subtle warmth where lit

    vec3 rgb = vColor.rgb * hdrGain * bright * tint;

    FragColor = vec4(rgb * a, a);
}
