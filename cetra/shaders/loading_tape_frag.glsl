#version 330 core

// The loading screen's tape (spec 13.34): the mark as a worn videotape plays it on an old set --
// the colour carried a little apart from the brightness, a tracking band rolling down the
// picture, lines that will not quite hold still, the head-switching tear along the bottom, static,
// and the set switching on and off -- and the tube's bloom, which is the mark's only glow, so it
// is the glow of the picture as it stands. Display-encoded out, as the CRT takes its picture.

in vec2 TexCoords;
out vec4 FragColor;

#include "display.glsl"
#include "noise.glsl"

uniform sampler2D markTex;  // the mark, linear
uniform sampler2D bloomTex; // the mark blurred, at a quarter its size (loading_blur_frag)
uniform float time;        // seconds since the screen was shown
uniform float off;         // the switch-off, 0 = on .. 1 = dark
uniform int frame;         // a count of the screen's draws, for noise new with each
uniform vec2 resolution;   // the picture's pixels

const float ON_START = 0.05;   // the set's line appears
const float ON_OPEN = 0.4;     // and opens to the full picture over this long
const float LOCK = 0.9;        // the static has gone by here
const float CHROMA = 0.0028;   // the colour's shift across, in picture widths
const float BAND_SPEED = 0.06; // the tracking band's travel, picture heights a second
const float BAND_H = 0.07;
const float JITTER = 0.0012;   // a line's wander
const float TEAR_H = 0.035;    // the head-switching tear at the bottom
const float STATIC = 0.05;  // the trace left once the picture locks, in display codes
const float GLITCH_EVERY = 5.0; // seconds between tracking glitches, on average
const float GLITCH_LONG = 0.22;
// The tube's bloom: how much of the blurred picture is added over it.
const float BLOOM = 0.6;

vec3 tapeAt(vec2 uv, float shift)
{
    float r = textureLod(markTex, uv + vec2(CHROMA + shift, 0.0), 0.0).r;
    float g = textureLod(markTex, uv, 0.0).g;
    float b = textureLod(markTex, uv - vec2(CHROMA + shift, 0.0), 0.0).b;
    return vec3(r, g, b);
}

void main()
{
    vec2 uv = TexCoords;
    float line = floor(uv.y * resolution.y);

    // Switching on: a bright line across the middle opens to the picture. Switching off: the
    // picture closes to a line, then the line to a point, which fades.
    float open = smoothstep(ON_START, ON_START + ON_OPEN, time);
    float closeY = 1.0 - smoothstep(0.0, 0.55, off);
    float closeX = 1.0 - smoothstep(0.5, 0.85, off);
    float height = max(min(open, closeY), 0.004);
    float width = max(closeX, 0.003);
    vec2 c = (uv - 0.5) / vec2(width, height);
    if (abs(c.y) > 0.5 || abs(c.x) > 0.5 || off >= 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float squeeze = min(1.0 / height, 6.0); // a squeezed picture: the same light, a thinner beam
    float fadeDot = 1.0 - smoothstep(0.8, 1.0, off);
    uv = c + 0.5;

    // A glitch now and then: a short burst where tracking is lost, its time drawn from the hash
    // of its slot so a run repeats.
    float slot = floor(time / GLITCH_EVERY);
    float at = slot * GLITCH_EVERY + hash21(vec2(slot, 7.0), vec2(12.9898, 78.233)) * (GLITCH_EVERY - GLITCH_LONG);
    float glitch = time > LOCK ? smoothstep(0.0, 0.03, time - at) * (1.0 - smoothstep(GLITCH_LONG - 0.05, GLITCH_LONG, time - at)) : 0.0;

    // The tracking band rolling down, and the lines inside it torn sideways.
    float bandY = 1.0 - fract(time * BAND_SPEED);
    float inBand = 1.0 - smoothstep(0.0, BAND_H * (1.0 + 2.0 * glitch), abs(uv.y - bandY));
    float lineNoise = frameNoise(uvec2(0u, uint(line)), uint(frame));
    float shift = (lineNoise - 0.5) * (JITTER + inBand * 0.02 + glitch * 0.03);
    // The head-switching tear: the bottom lines run off to the right, more toward the edge.
    float tear = 1.0 - smoothstep(0.0, TEAR_H, uv.y);
    shift += tear * tear * (0.03 + 0.02 * lineNoise);
    vec2 src = vec2(uv.x + shift, uv.y); // where this line of the picture is read from
    vec3 col = tapeAt(src, glitch * 0.004) + BLOOM * textureLod(bloomTex, src, 0.0).rgb;

    // Static: all of it while the set warms up, a trace after, more in the band and the tear.
    float grain = frameNoise(uvec2(gl_FragCoord.xy), uint(frame));
    float warm = 1.0 - smoothstep(ON_START, LOCK, time);
    // The set's own falloff toward its edges, and the squeezed beam's brightness.
    vec2 v = uv - 0.5;
    col *= (1.0 - 0.9 * dot(v, v)) * mix(1.0, squeeze, 1.0 - open * closeY) * fadeDot;

    // Static is laid on in display codes, as a signal's noise is: in light, the faintest of it
    // lifts black to a grey once encoded.
    vec3 code = displayEncode(max(col, 0.0));
    float amount = 0.7 * warm + 0.12 * inBand + 0.2 * tear + 0.15 * glitch;
    code = mix(code, vec3(grain), clamp(amount, 0.0, 1.0) * fadeDot);
    code += (grain - 0.5) * STATIC * fadeDot;
    FragColor = vec4(clamp(code, 0.0, 1.0), 1.0);
}
