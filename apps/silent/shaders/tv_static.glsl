#version 330 core

// The living-room set with no station on it (spec 13.30): analog snow, the receiver's own noise
// turned up by its AGC until it fills the picture. Drawn in the late draw, past TAA, because a new
// field every 1/59.94 s is what a history averages into grey. The glass under it already emits the
// signal's mean in the tube's shape and under the hum bar, so this adds the noise about that mean.

in vec2 vUv;
in float vViewDepth;
out vec4 FragColor;

#include "late_surface.glsl"
#include "pcg4d.glsl"

// TV_PICTURE_CHUNK

uniform vec4 tvSignal; // lines down the picture, samples along a line, the black level, the gamma
uniform vec4 tvWhite;  // the white at peak, nits and linear; w = the picture's width over its height

// A pixel's footprint is averaged over at most this many taps a side.
const float TV_TAPS = 8.0;

// The receiver's noise at sample `s` of line `line` in field `field`: a unit Gaussian, from two of
// the hash's lanes (Box-Muller).
float snowSample(int s, int line, uint field)
{
    uvec2 h = pcg4d(uvec4(uint(s), uint(line), field, 0u)).xy;
    float u1 = (float(h.x >> 8u) + 1.0) * (1.0 / 16777216.0); // (0, 1], for the log
    float u2 = float(h.y >> 8u) * (1.0 / 16777216.0);
    return sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2);
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

// The tube's light at `lineF` lines down and `x` samples across, as a fraction of peak and before
// the hum bar: the signal clipped at black and white and through the gamma, under the beam, drawn
// in by `beam` of the way.
float snowLight(float lineF, float x, uint field, float beam)
{
    int line = int(floor(lineF));
    // Interlaced: a line holds the last field of its own parity; the one before has faded.
    uint drawn = field - ((field ^ uint(line)) & 1u);
    float v = clamp(tvSignal.z + tvField.z * snowLine(x, line, drawn), 0.0, 1.0);
    // The beam across its line, a raised cosine whose mean over the line is 1: it moves no light.
    float across = 1.0 + cos(6.2831853 * (fract(lineF) - 0.5));
    return pow(v, tvSignal.w) * mix(1.0, across, beam);
}

void main()
{
    // Line 0 at the top, as a set scans.
    float lineF = (1.0 - vUv.y) * tvSignal.x;
    float x = vUv.x * tvSignal.y;
    uint field = uint(tvField.x);

    // The pixel's footprint in lines and samples, averaged as the eye averages a set across the
    // room. Past TV_TAPS a side the taps taken stand in for the whole footprint, and their spread
    // about the mean is narrowed to the footprint's, so the grain's contrast is the same at any
    // number of pixels to the picture.
    float dl = max(fwidth(lineF), 1e-4);
    float ds = max(fwidth(x), 1e-4);
    float nl = min(ceil(dl), TV_TAPS);
    float ns = min(ceil(ds), TV_TAPS);
    // The beam's lines show where a line covers two pixels or more; finer, they beat with them.
    float beam = smoothstep(1.5, 2.5, 1.0 / dl);
    float light = 0.0;
    for (float j = 0.5; j < nl; j += 1.0)
        for (float k = 0.5; k < ns; k += 1.0)
            light += snowLight(lineF + (j / nl - 0.5) * dl, x + (k / ns - 0.5) * ds, field, beam);
    light /= nl * ns;
    float mean = tvField.w;
    light = mean + (light - mean) * sqrt(nl / max(dl, nl) * ns / max(ds, ns));

    vec3 emitted = tvWhite.rgb * (light - mean) * tvHum(vUv.y) * tvTubeShape(vUv, tvWhite.w);
    // A few millimetres of slack: the card lies on the glass, whose depth it is tested against.
    FragColor = vec4(lateEmit(emitted, vViewDepth) * lateVisible(vViewDepth, 0.004), 0.0);
}
