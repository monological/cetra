#version 330 core

// Falling rain (spec 13.9), composited premultiplied onto the HDR canvas after the temporal
// seam: blend ONE, ONE_MINUS_SRC_ALPHA. See rain_vert.glsl.

in float vAcrossPx;
in float vHalfWidth;
in vec3 vLit;
in float vAlpha;
in float vViewDepth;
in float vAlongPx;
flat in float vLengthPx;
flat in float vGlintBands;
flat in float vGlintPhase;
flat in float vGlintShare;

out vec4 FragColor;

uniform mat4 projection;      // read by depth.glsl
uniform vec2 viewport;        // post-resolution pixels
uniform sampler2D sceneDepth; // resolved scene depth, at RENDER resolution

// The frame before any drop drew, mipped, and the level a drop reads it at. A drop refracts
// a field of about 165 degrees (Garg and Nayar 2003), so what it shows is the frame's AVERAGE
// round it, not the pixel behind it -- which is why rain in front of a lit wall is a faint
// darkening and rain in front of a dark one a faint brightening, and neither is a black line.
uniform sampler2D behindTex;
uniform float behindLod;

// The fog the frame integrated, so a drop is fogged at its own depth rather than at the
// surface behind it. fogSlices 0 = none this frame, which reads as the identity.
uniform sampler3D fogVolume;
uniform int fogSlices;
uniform float fogNear;
uniform float fogFar;
uniform float fogDepthDist;

#include "froxel.glsl"

// Metres over which a streak fades into the surface behind it, so a drop reaching the
// ground dissolves into it instead of ending on a hard line.
#define RAIN_SOFT_DEPTH 0.05

const float PI = 3.14159265359;

// How sharp a glint is, and the mean of ((1 + cos) / 2)^p over a cycle, (2p-1)!! / (2p)!!,
// which the profile is divided by so that it averages 1: glints REDISTRIBUTE the lit light
// along the streak rather than adding any.
#define RAIN_GLINT_POWER 8.0
#define RAIN_GLINT_MEAN 0.196380615

// Rayleigh's n = 3 mode against the n = 2 one: sqrt(n(n-1)(n+2)) is sqrt(30) against sqrt(8).
#define RAIN_MODE3_RATIO 1.93649167

float glintMode(float bands, float phase, float s) {
    float c = 0.5 + 0.5 * cos(2.0 * PI * (bands * s + phase));
    return pow(c, RAIN_GLINT_POWER) / RAIN_GLINT_MEAN;
}

// A drop rings in more than one mode, which is what keeps its flashes from spacing out as
// evenly as a dotted line. Each profile averages 1, so any mix of the two does too.
float glintProfile(float s) {
    float w = fract(vGlintPhase * 5.3);
    return mix(glintMode(vGlintBands, vGlintPhase, s),
               glintMode(vGlintBands * RAIN_MODE3_RATIO, vGlintPhase * 1.7, s), 0.25 + 0.5 * w);
}

void main() {
    // The width profile integrates to the drawn width, so a one-pixel streak carries exactly
    // the coverage the vertex stage gave it and its edges are antialiased.
    float a = vAlpha * clamp(vHalfWidth + 0.5 - abs(vAcrossPx), 0.0, 1.0);
    vec2 uv = gl_FragCoord.xy / viewport;
    float surface = -viewZFromNdcZ(2.0 * texture(sceneDepth, uv).r - 1.0);
    a *= clamp((surface - vViewDepth) / RAIN_SOFT_DEPTH, 0.0, 1.0);
    if (a <= 0.0)
        discard;
    // The drop's own light is dimmed by the air between it and the eye, and added to by that
    // air's in-scatter. What it refracts needs neither: the frame already carries its fog.
    vec4 front = froxelSampleMedium(fogVolume, uv, vViewDepth, fogNear, fogFar, fogSlices,
                                    fogDepthDist);
    vec3 behind = textureLod(behindTex, uv, behindLod).rgb;
    // Only the lit half flashes: the glint is the lamp's image in the drop, and what it
    // refracts is smooth.
    float s = clamp(vAlongPx / max(vLengthPx, 1.0), 0.0, 1.0);
    vec3 lit = vLit * mix(1.0, glintProfile(s), vGlintShare);
    FragColor = vec4(a * (lit * front.a + behind), a);
}
