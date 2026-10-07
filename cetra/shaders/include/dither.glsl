// The output dither (spec 11.24): triangular-PDF noise of one LSB laid over a display-encoded
// colour just before it is quantized to the 8-bit target, by the pass that writes the window, as
// its last step: anything after it reaches the target undithered.

#include "noise.glsl"

// Triangular-PDF dither in (-1, 1) LSB, one independent sample per channel.
//
// The subtrahend swaps and SCALES the coordinates rather than offsetting them.
// ign() is a sawtooth riding a linear ramp in p, so a constant offset shifts
// only that sawtooth's phase: ign(p) - ign(p + c) is two sawtooths at one
// frequency and collapses to four distinct values -- a staircase, not a dither.
// Changing the scale changes the frequency, which is what makes the taps
// independent.
//
// Each channel varies in both the swap and the scale for the same reason. Three
// channels sharing a frequency would dither only along the grey axis, leaving a
// colour gradient's contours standing in whichever channel banded elsewhere.
float ditherTap(vec2 p, float sa, vec2 oa, float sb, vec2 ob)
{
    return ign(p * sa + oa) - ign(p.yx * sb + ob);
}

// The scale pairs read down the column -- 1.00/1.37, 1.61/0.83, 0.71/2.13 --
// so a channel that lost its frequency split is visible without re-deriving it.
vec3 ditherPattern(vec2 p)
{
    return vec3(ditherTap(p, 1.00, vec2(0.0, 0.0), 1.37, vec2(19.0, 7.0)),
                ditherTap(p, 1.61, vec2(7.0, 31.0), 0.83, vec2(53.0, 11.0)),
                ditherTap(p, 0.71, vec2(31.0, 3.0), 2.13, vec2(97.0, 61.0)));
}

// `color` dithered at window pixel `pixel`, `strength` LSB of swing.
//
// Static, with no frame term. An animated pattern would put roughly half the
// frame's pixels 1 LSB apart between any two consecutive frames, and that
// lands in every temporal-churn measurement taken off the final framebuffer
// -- including the no-feature floor arms those measurements are scaled
// against, so both sides would inflate together and the comparison would
// stop discriminating.
//
// Faded out where the signal has no room for the full swing. A value
// already at 0 or 1 carries no quantization error to decorrelate, and the
// clamp below would keep only one half of the TPDF there -- flat white
// stipples to 254 and flat black lifts off zero, which is a DC bias, not
// noise. `room` is the distance to the nearer endpoint measured in units of
// the dither's own amplitude, so full swing resumes as soon as the
// distribution fits, and the fade widens correctly as the strength rises.
vec3 applyDither(vec3 color, vec2 pixel, float strength)
{
    float amp = strength * (1.0 / 255.0);
    vec3 room = clamp(min(color, 1.0 - color) / max(amp, 1e-6), 0.0, 1.0);
    return clamp(color + ditherPattern(pixel) * amp * room, 0.0, 1.0);
}
