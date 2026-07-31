
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
// Only the CAMERA half pre-exposes the scene. Auto-exposure's adaptation gain is
// still applied at the tonemap, which means the HDR buffer is up to 6 stops off
// "1.0 is white" whenever adaptation is doing work -- so bloom's threshold and
// the WS_* ceilings are mis-scaled by exactly that much.
//
// That is a known defect, not an oversight, and the obvious fix does not work:
// pre-exposing by the gain puts the adaptation loop downstream of the
// working-space clamps that shading applies (pbr_frag's WS_LIGHT_MAX, the 0.03
// ambient floor). Those are constant in working space, so the meter -- which
// divides pre-exposure back out to read absolute radiance -- sees them GROW as
// the gain closes, and the exposure ratchets down instead of settling. Measured:
// lum rises exactly as 1/gain, ~1%/frame with the ambient floor live and
// ~0.07%/frame without. See specs/10.1 for the shape of a real fix.
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
    // The adapted value itself lives on the GPU (postfx's 1x1 lum_adapt target)
    // because that is where it is both produced and consumed. Nothing on the CPU
    // reads it, which is exactly the limitation described above.
} Exposure;

Exposure* create_exposure(void);
void free_exposure(Exposure* ex);

// EV100 = log2(N^2 / t * 100 / ISO), the photographic definition.
float exposure_ev100(const Exposure* ex);

// The camera's multiplier: the manual value, or the physical camera. Free of
// scene content, so it is identical across runs, which is what pinned-exposure
// goldens ride on. Currently the whole of what pre-exposes a frame.
float exposure_camera_multiplier(const Exposure* ex);

#endif // _EXPOSURE_H_
