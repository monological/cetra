
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
// leave the buffer up to 64x off the scale its own thresholds assume.
typedef struct Exposure {
    // false = `bias` is the linear multiplier it always was, which is what every
    // scene authored before the physical camera existed still gets.
    //
    // true = aperture/shutter/iso produce the multiplier and `bias` becomes a
    // stops offset instead -- the meaning it already carries under `automatic`,
    // so the field never has two. Photometric lights are the wrong magnitude for
    // a hand-picked multiplier: a 127 cd bulb at 2 m is ~32 lux, and nothing
    // about "0.7846" tells you whether that lands mid-grey. Aperture, shutter
    // and ISO do, being the numbers a camera in that room would use.
    bool physical;
    float aperture;      // f-number (f/2.8 -> 2.8)
    float shutter_speed; // seconds (1/60 s -> 0.01667)
    float iso;           // ISO sensitivity (100, 400, ...)
    float bias;          // linear multiplier, or stops when physical/automatic

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

// The deterministic half: the manual multiplier, or the physical camera. Free of
// scene content, so it is identical across runs and is what pinned-exposure
// goldens ride on.
float exposure_camera_multiplier(const Exposure* ex);

// The adaptation half, in [1/64, 1]. Exactly 1.0 when auto-exposure is off or has
// not metered yet, so the two halves multiply unconditionally.
float exposure_auto_gain(const Exposure* ex);

// camera x adaptation -- the whole exposure, and what pre-exposes the frame.
float exposure_multiplier(const Exposure* ex);

// Hand back a frame's metered mean (absolute radiance). Values that cannot be a
// luminance are refused rather than latched, since a NaN here would propagate
// into the pre-exposure and blank every subsequent frame.
void exposure_set_adapted_luminance(Exposure* ex, float luminance);

// Drop the adaptation history, so the next metered frame snaps instead of
// blending from a value measured under different conditions.
void exposure_reset_adaptation(Exposure* ex);

#endif // _EXPOSURE_H_
