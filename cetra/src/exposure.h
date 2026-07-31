
#ifndef _EXPOSURE_H_
#define _EXPOSURE_H_

#include <stdbool.h>

// The camera's exposure, and the one place that decides what it is.
//
// Exposure used to be four things in three files: a linear multiplier on PostFX,
// an EV100 camera added by spec 10.0, an adaptation gain metered in two shaders
// and applied in a third, and render.c's ViewParams publish picking one of them.
// Nothing was wrong individually; there was just no single answer to "what is
// the exposure this frame", which is precisely the question the working-space
// contract (shaders/include/view.glsl) is stated in terms of.
//
// The whole exposure is exposure_multiplier(): camera x adaptation. That is what
// pre-exposes the scene, so 1.0 in the HDR buffer is diffuse white no matter
// which mechanism produced it -- and every WS_* threshold, and bloom's, then
// means what it says. Applying the adaptation gain later, at the tonemap, would
// leave the buffer off the scale its own thresholds assume by however far the
// adaptation had travelled -- up to 20 stops on a sunlit scene.
//
// Pre-exposing by the gain only became possible once nothing upstream of the
// meter was a fixed multiple of WHITE (spec 10.1 phase 5). While pbr_frag still
// clamped nits and carried a 3%-of-white ambient floor, those terms measured
// brighter as the gain closed -- the meter divides pre-exposure back out -- and
// the exposure ratcheted instead of settling. Anything added upstream of the
// metering pass has to be a fixed multiple of RADIANCE, or that returns.
typedef struct Exposure {
    // Selects which of the two fields below is the camera. Photometric lights
    // are the wrong magnitude for a hand-picked multiplier -- a 127 cd bulb at
    // 2 m is ~32 lux, and nothing about "0.7846" tells you whether that lands
    // mid-grey. Aperture, shutter and ISO do, being the numbers a camera in
    // that room would use. Off by default so scenes authored before the
    // physical camera existed keep their multiplier.
    bool physical;
    float aperture;      // f-number (f/2.8 -> 2.8), physical only
    float shutter_speed; // seconds (1/60 s -> 0.01667), physical only
    float iso;           // ISO sensitivity (100, 400, ...), physical only

    // Two genuinely different quantities, because they are read on the two
    // sides of `physical` and nothing sensible converts between them. They were
    // one field, `bias`, whose comment claimed the meaning switched on
    // `physical || automatic`; it switched on `physical` alone, and the GUI
    // slider drove it through one hardcoded linear range either way -- so under
    // an authored camera it was stops, could not reach 0, and could not stop
    // down. spores is the case that proves the old claim wrong: automatic on,
    // physical off, and it relies on a linear 0.15.
    float multiplier; // linear, when !physical
    float bias_stops; // EV offset, when physical (positive opens up)

    bool automatic; // adapt to the scene's metered luminance
    float key;      // middle grey the metered mean is mapped to (0.18)

    // Last frame's metered geometric-mean luminance, in ABSOLUTE scene radiance
    // (the meter divides the pre-exposure back out). One frame stale by
    // construction: this frame's value cannot exist before this frame is shaded,
    // and shading needs the exposure. The lag is far below the adaptation time
    // constant, so it is invisible; the alternative is a circular dependency.
    //
    // `adapted_valid` is false until a frame has actually been metered, which is
    // what keeps the first frame from adapting to uninitialised memory.
    float adapted_luminance;
    bool adapted_valid;
} Exposure;

Exposure* create_exposure(void);
void free_exposure(Exposure* ex);

// EV100 = log2(N^2 / t * 100 / ISO), the photographic definition.
float exposure_ev100(const Exposure* ex);

// The camera's multiplier: the manual value, or the physical camera. Free of
// scene content, so it is identical across runs, which is what pinned-exposure
// goldens ride on.
float exposure_camera_multiplier(const Exposure* ex);

// The adaptation half: key / metered luminance, capped at 1 (it only ever
// darkens) and floored 20 stops down, which is the range absolute nits need.
// Exactly 1.0 when auto-exposure is off or has not metered yet, so the two
// halves multiply unconditionally.
float exposure_auto_gain(const Exposure* ex);

// camera x adaptation -- the whole exposure, and what pre-exposes the frame.
float exposure_multiplier(const Exposure* ex);

// Hand in a frame's raw metered mean as log2 luminance, and blend the adapted
// value toward it -- eye adaptation. Rate is per FRAME, not per second, so a
// headless run of N frames adapts identically every time; that determinism is
// why this is not driven off the frame clock.
//
// Values that cannot be a luminance are refused rather than latched, since a
// NaN here would propagate into the pre-exposure and blank every later frame.
void exposure_submit_measurement(Exposure* ex, float log2_luminance);

// Drop the adaptation history, so the next metered frame snaps instead of
// blending from a value measured under different conditions.
void exposure_reset_adaptation(Exposure* ex);

#endif // _EXPOSURE_H_
