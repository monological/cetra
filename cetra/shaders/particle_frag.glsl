#version 330 core

// M3 particle shading: soft HDR sprite lit by the directional key and dimmed
// inside the scene's CSM shadow, so motes brighten in the light and fall dark
// in shadow. Output is premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
// See specs/5.0-particle-system.md.

in vec2 vCorner;
in vec4 vColor;
in float vLifeFrac;
in vec3 vWorldPos;
in float vViewZ;

layout(location = 0) out vec4 FragColor;

uniform float hdrGain;    // push color above 1.0 so bloom catches the motes
uniform vec3 uSunColor;   // key light color (subtle warm tint where it hits)
uniform float uAmbient;   // brightness floor for shadowed motes
uniform mat4 projection;  // for soft-particle depth linearization

// Soft particles (M4): fade the sprite as it approaches the opaque surface
// behind it, so billboards don't show a hard edge where they cut into geometry.
uniform sampler2D sceneDepth; // resolved single-sample scene depth
uniform int uSoftEnabled;     // 0 when no depth texture is bound
uniform float softDist;       // world-space fade band

// Optional sprite (billboard_renderer_set_sprite): a texture with its own shape
// and cutout replaces the procedural disc. Everything downstream -- life fade,
// soft particles, sun/shadow lighting -- applies to both paths alike.
uniform sampler2D uSpriteTex;
uniform int uSpriteEnabled;

// Window-space depth [0,1] -> positive view-space distance from the camera.
float sceneViewDist(float d) {
    float zndc = 2.0 * d - 1.0;
    float zeye = projection[3][2] / (-zndc - projection[2][2]);
    return -zeye;
}

// 3x3 PCF: cheaper than the catcher's, and motes are small enough that the
// wider kernel buys nothing.
#define CSM_OUTERMOST_PCF
#define CSM_PCF_HALF_KERNEL 1
#include "csm.glsl"

float occlusion_from(int slot) {
    return csmOutermostOcclusion(vWorldPos, slot);
}

void main() {
    vec3 baseRgb = vColor.rgb;
    float a;
    if (uSpriteEnabled == 1) {
        vec4 s = texture(uSpriteTex, vCorner * 0.5 + 0.5);
        if (s.a < 0.02)
            discard; // outside the sprite's cutout
        baseRgb *= s.rgb;
        a = s.a * vColor.a;
    } else {
        // Soft round sprite from the radial distance across the quad.
        a = smoothstep(1.0, 0.35, length(vCorner)) * vColor.a;
    }

    // Life alpha envelope: fade in over the first 10%, out over the last 30%.
    float fadeIn = smoothstep(0.0, 0.1, vLifeFrac);
    float fadeOut = 1.0 - smoothstep(0.7, 1.0, vLifeFrac);
    a *= fadeIn * fadeOut;

    // Soft particles: fade as the mote nears the opaque surface behind it.
    if (uSoftEnabled == 1) {
        vec2 uv = gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0));
        float sceneDist = sceneViewDist(texture(sceneDepth, uv).r);
        float partDist = -vViewZ; // positive distance from camera
        a *= clamp((sceneDist - partDist) / softDist, 0.0, 1.0);
    }

    // Motes brighten in the key, fall to the ambient floor in shadow. Billboards
    // have no meaningful normal, so this is a light-vs-shadow term, not N.L.
    float occ = (numShadowLights > 0) ? occlusion_from(0) : 0.0;
    float sun = 1.0 - occ;
    float bright = uAmbient + (1.0 - uAmbient) * sun;
    vec3 tint = mix(vec3(1.0), uSunColor, 0.4 * sun); // subtle warmth where lit

    vec3 rgb = baseRgb * hdrGain * bright * tint;

    FragColor = vec4(rgb * a, a);
}
