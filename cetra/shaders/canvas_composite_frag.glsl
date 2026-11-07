#version 330 core

// A canvas view into the frame it is drawn over (spec 14.0): the view's own target, copied into
// the view's rectangle, opaque. The viewport is that rectangle, so this runs only inside it. While
// the view fades (spec 14.24), each tagged pixel fades toward the view's colour, less near a light.

#include "canvas_constants.glsl"

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uView;
uniform ivec4 uOrigin;  // xy: the rectangle's lower left corner in the frame, framebuffer pixels
uniform sampler2D uLit; // the lit pass's: r a lit shape's coverage, g any tagged shape's
uniform float uFade;    // how far a tagged pixel away from a light fades; 0 = none
uniform vec3 uFadeTo;   // toward this colour
uniform float uRoom;    // framebuffer pixels round a light the fading eases off over

const float TAU = 6.2831853;
// A tap reads the lit target at a level as wide as its ring's spacing, which averages a light
// thinner than that down; so a quarter of a tap's footprint lit counts as wholly lit.
const float TAP_GAIN = 4.0;

// How lit a pixel is, from the light on it: 1 on a light; within uRoom of one, easing to 0 at
// uRoom, by the largest of CANVAS_FADE_RINGS rings of CANVAS_FADE_SPOKES taps round it.
float lightAt(ivec2 p, float here) {
    if (uRoom < 1.0 || here >= 1.0)
        return here;
    vec2 size = vec2(textureSize(uLit, 0));
    vec2 uv = (vec2(p) + 0.5) / size;
    float lit = here;
    for (int r = 1; r <= CANVAS_FADE_RINGS; r++) {
        float radius = uRoom * float(r) / float(CANVAS_FADE_RINGS);
        float lod = log2(max(TAU * radius / float(CANVAS_FADE_SPOKES), 1.0));
        float inner = uRoom * float(r - 1) / float(CANVAS_FADE_RINGS);
        float falloff = 1.0 - smoothstep(0.0, uRoom, inner);
        for (int s = 0; s < CANVAS_FADE_SPOKES; s++) {
            float a = TAU * (float(s) + 0.5 * float(r & 1)) / float(CANVAS_FADE_SPOKES);
            vec2 at = uv + radius * vec2(cos(a), sin(a)) / size;
            float v = clamp(textureLod(uLit, at, lod).r * TAP_GAIN, 0.0, 1.0);
            lit = max(lit, v * falloff);
        }
    }
    return lit;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - uOrigin.xy;
    vec3 colour = texelFetch(uView, p, 0).rgb;
    if (uFade > 0.0) {
        vec2 lit = texelFetch(uLit, p, 0).rg;
        if (lit.g > 0.0)
            colour = mix(colour, uFadeTo, uFade * lit.g * (1.0 - lightAt(p, lit.r)));
    }
    FragColor = vec4(colour, 1.0);
}
