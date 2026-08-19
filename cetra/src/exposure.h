
#ifndef _EXPOSURE_H_
#define _EXPOSURE_H_

#include <stdbool.h>

// How the frame is weighted before the percentiles are taken.
//
// The weight multiplies a texel's contribution to the POPULATION, not to the
// value, so a half-weighted texel is half a vote rather than half as bright.
// That is what keeps a mode change from moving the metered value of a flat
// frame: weighting a uniform scene any way at all still meters the same
// luminance, which is the property a mode has to have to be a framing choice
// rather than an exposure offset.
typedef enum MeteringMode {
    METERING_UNIFORM = 0, // every texel votes equally
    METERING_CENTRE,      // smooth falloff toward the edges
    METERING_SPOT,        // a central disc votes; nothing else does
} MeteringMode;

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

    // Fraction of the population kept, by luminance order: everything below
    // `meter_low` and above `meter_high` is discarded before the mean is taken.
    //
    // Over POPULATION, not over the value range, which is what makes them
    // survive the scene being scaled -- "ignore the darkest tenth" holds at any
    // brightness where "ignore anything under 0.18 nits" does not. That
    // distinction is the whole reason these exist: the absolute floor they
    // replaced cost 1.61 stops of scale-covariance on a dim scene (spec 11.52).
    //
    // The low default is BACKGROUND REJECTION, not a tail trim -- 0.70 discards
    // most of the population, and it has to. Where no geometry was drawn the
    // buffer holds exactly 0, and zero times a thousand is still zero, so that
    // population never scales with the scene; a meter that keeps it is metering
    // the background. exposure_init carries the measurements. Setting a mild
    // 0.02 expecting an analogous trim gets a meter dominated by the void, which
    // is the reading this comment used to invite.
    float meter_low;
    float meter_high;

    // Which part of the frame votes. Uniform by default -- a metering mode is a
    // framing decision and nothing in this engine's fixtures is framed for one.
    MeteringMode meter_mode;
    // Radius of the spot / the centre weighting's falloff, as a fraction of the
    // frame's half-diagonal. Measured in UV, so on a non-square frame the disc is
    // an ellipse in pixels -- the same convention a metering mask texture has.
    float meter_radius;

    // Bounds on the metered luminance, as log2 cd/m^2 (UE's Min/Max Brightness).
    // Inert by default -- wider than the histogram's own range at both ends, so
    // nothing is clamped until a scene asks for it. Their job is to stop a
    // pathological frame (a camera inside geometry, a fully black loading frame)
    // walking the exposure somewhere it cannot walk back from within the
    // adaptation rate.
    //
    // Settable in code only for now; no CLI flag, .cscn key or slider reaches
    // them, unlike the six knobs above. Said plainly because the defaults being
    // inert is what makes that gap survivable rather than invisible.
    float meter_min_log2;
    float meter_max_log2;

    // Per-FRAME blend rates, separately for a scene getting brighter and one
    // getting darker -- a real eye dilates slower than it contracts, and UE
    // splits these for the same reason.
    //
    // Per frame and not per second: a headless run of N frames must adapt
    // identically every time, which is why this is not driven off the frame
    // clock. Equal by default, so the split costs nothing until asked for.
    float adapt_rate_up;
    float adapt_rate_down;

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

    // What the last submitted measurement was, and what it became after the
    // metered bounds were applied. Diagnostic, but they are the only record:
    // exposure_submit_measurement folds the reading into adapted_luminance and
    // drops both, so nothing outside that call could otherwise see either.
    //
    // `last_target_log2` is the value the adaptation is actually converging TO.
    // Comparing `adapted` against the RAW reading instead looks equivalent and is
    // not -- on a frame the bounds clamp, the two never meet however long it
    // runs, which reads as an adaptation that never settles.
    float last_raw_log2;
    float last_target_log2;

    // Print what the meter decided, every frame it decides it. Diagnostic only;
    // nothing downstream reads it.
    //
    // It lives here rather than on PostFX because this struct is the one place
    // that decides what the exposure is, and a report of that decision made
    // anywhere else would be reading the pieces back out and re-deriving it.
    bool probe;
} Exposure;

// Initialise in place. No allocation: an Engine always has exactly one, so a
// pointer would only invite the NULL checks that had four different answers.
void exposure_init(Exposure* ex);

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

// One line per metered frame, on stdout, in the --water-fft-probe idiom.
//
// `raw_log2` is the measurement as it arrived, BEFORE the blend -- the one
// number in the whole path that exists nowhere afterwards, since
// exposure_submit_measurement folds it into adapted_luminance and drops it. The
// rest is read back off `ex`, so the report cannot describe an exposure the
// engine is not using.
//
// Call it AFTER submitting, so `adapted` and `gain` are this frame's. Costs a
// printf on a frame nobody is measuring, which is why it is behind `probe`.
void exposure_probe_report(const Exposure* ex, float raw_log2, int frame);

#endif // _EXPOSURE_H_
