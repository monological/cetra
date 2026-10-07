#version 330 core

// The living-room set with no station on it (spec 13.30): analog snow, the receiver's own noise
// turned up by its AGC until it fills the picture. Drawn in the late draw, past TAA, because a new
// field every 1/59.94 s is exactly what a history averages into grey.
//
// What it draws, from how a set makes it:
//   - a field per 1/59.94 s, interlaced: each line holds the field that last drew it
//   - per line, ~440 independent values, band-limited along the line by a cubic B-spline, so a
//     speck is a short horizontal dash; the lines are independent of each other
//   - monochrome, the colour killer having nothing to lock to, at the tube's cool white
//   - Gaussian noise about a black level, clipped, through the tube's 2.4 gamma: dark, with
//     bright specks
//   - a hum bar, mains beating against the field rate, rolling up the picture
//   - the tube: rounded corners, a slight falloff to the edges, and the beam's line structure
//     where a line covers two pixels or more
//
// The glass under it carries this picture's MEAN as its own emission, so everything before the
// late draw -- the floor's reflection, the probes, TAA's history -- sees a lit screen. This adds
// what the static is beyond that mean, and takes the mean away outside the corners.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
in float vViewDepth;
out vec4 FragColor;

#include "late_surface.glsl"
#include "noise.glsl"

uniform vec4 tvSignal; // lines down the picture, samples along a line, fields a second
uniform vec4 tvTube;   // peak white (nits), this field's AGC gain, the mean the glass emits
                       // (nits), the corners' radius as a fraction of the picture's height
uniform vec4 tvSnow;   // black level, noise sigma at a gain of 1, hum bar depth, its period (s)
uniform vec4 tvTint;   // the phosphor white, linear; w = the picture's width over its height

// One value of the receiver's noise, unit variance, from three uniforms (Irwin-Hall): sample `s`
// of line `line` in field `field`.
float snowSample(int s, int line, uint field)
{
    uvec3 h = pcg4d(uvec4(uint(s), uint(line), field, 0u)).xyz;
    vec3 u = vec3(h >> 8u) * (1.0 / 16777216.0);
    return (u.x + u.y + u.z - 1.5) * 2.0;
}

// Line `line` at `x` samples along it, through a cubic B-spline over four samples -- the video
// bandwidth drawing each speck into a dash -- renormalised to unit variance, which the spline's
// smoothing otherwise takes down by a third and by how far between samples `x` falls.
float snowLine(float x, int line, uint field)
{
    float i = floor(x - 0.5);
    float t = x - 0.5 - i;
    float t2 = t * t, t3 = t2 * t;
    vec4 w = vec4((1.0 - t) * (1.0 - t) * (1.0 - t), 3.0 * t3 - 6.0 * t2 + 4.0,
                  -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0;
    int s = int(i);
    float sum = w.x * snowSample(s - 1, line, field) + w.y * snowSample(s, line, field) +
                w.z * snowSample(s + 1, line, field) + w.w * snowSample(s + 2, line, field);
    return sum / sqrt(dot(w, w));
}

// The tube's light at `lineF` lines down and `x` samples across, relative to its peak white.
float snowLight(float lineF, float x, float pxPerLine)
{
    int line = int(floor(lineF));
    uint field = uint(floor(time * tvSignal.z));
    // Interlaced: odd lines on odd fields. The other half of the lines still holds the field
    // before, and the phosphor's few milliseconds leave nothing of the one before that.
    uint drawn = (uint(line) & 1u) == (field & 1u) ? field : field - 1u;
    float g = snowLine(x, line, drawn);
    // The hum bar: a broad soft band of lower gain, its phase moving down the lines so it rolls up.
    float band = 0.5 + 0.5 * cos(6.2831853 * (lineF / tvSignal.x + time / tvSnow.w));
    float v = clamp(tvSnow.x + tvSnow.y * tvTube.y * g, 0.0, 1.0) * (1.0 - tvSnow.z * band);
    float light = pow(v, 2.4);
    // The beam across its line, a Gaussian a quarter of a line wide, held at a mean of 1 so it
    // moves no light; visible only where a line spans two pixels or more, or it beats with them.
    float across = fract(lineF) - 0.5;
    float beam = exp(-across * across / 0.125) / 0.598;
    return light * mix(1.0, beam, smoothstep(1.5, 2.5, pxPerLine));
}

void main()
{
    // Line 0 at the top, as a set scans.
    float lineF = (1.0 - vUv.y) * tvSignal.x;
    float x = vUv.x * tvSignal.y;

    // The pixel's footprint in lines and samples. Where it spans several, as on a set across the
    // room, it averages them -- the eye does the same -- rather than picking one and aliasing.
    float dl = max(fwidth(lineF), 1e-4);
    float ds = max(fwidth(x), 1e-4);
    int nl = int(clamp(ceil(dl), 1.0, 3.0));
    int ns = int(clamp(ceil(ds), 1.0, 3.0));
    float light = 0.0;
    for (int j = 0; j < nl; j++) {
        for (int k = 0; k < ns; k++) {
            float lf = lineF + ((float(j) + 0.5) / float(nl) - 0.5) * dl;
            float xf = x + ((float(k) + 0.5) / float(ns) - 0.5) * ds;
            light += snowLight(lf, xf, 1.0 / dl);
        }
    }
    light /= float(nl * ns);

    // The tube: a rounded rectangle, soft at its edge, a little darker toward its sides.
    vec2 q = (vUv - 0.5) * vec2(tvTint.w, 1.0);
    vec2 extent = vec2(tvTint.w, 1.0) * 0.5;
    float r = tvTube.w;
    float d = length(max(abs(q) - (extent - r), 0.0)) - r;
    float inside = 1.0 - smoothstep(-0.01, 0.0, d);
    float falloff = 1.0 - 0.3 * dot(q / extent, q / extent) * 0.5;

    vec3 emitted = tvTint.rgb * (light * tvTube.x * inside * falloff - tvTube.z);
    // A few millimetres of slack: this card lies a millimetre in front of the glass.
    FragColor = vec4(lateEmit(emitted, vViewDepth) * lateVisible(vViewDepth, 0.004), 0.0);
}
