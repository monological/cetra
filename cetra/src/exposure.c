
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exposure.h"
#include "ext/log.h"

// How far the adaptation may stop DOWN, in stops. It bounds darkening, not
// brightening: gain = key / metered, so a dark frame yields a LARGE gain, which
// the 1.0 ceiling catches. This end only engages on bright scenes.
//
// It has to span the photometric range now that the meter reads absolute nits.
// Metered means run from ~1e-3 (starlight) to ~1e5 (direct sun); against a
// 0.18 key that is roughly 20 stops of stopping down. The old value was 6
// stops, from when the buffer was normalised so that ~1.0 was white -- at which
// point anything over 11.5 nits pinned here and rendered blown out. Measured on
// a sunlit-ish fixture metering 700 nits: the gain sat exactly on the old floor
// and could not move.
#define EXPOSURE_MAX_STOPS_DOWN 20.0f

// Default per-FRAME blend toward the measurement, and the deadband that follows
// it. Both used to live in lum_adapt_frag.glsl; they are policy, so they belong
// with the rest of it now that the value is read back to the CPU anyway. The
// rate is only the DEFAULT since 11.52 -- adapt_rate_up/down are the live
// values, and a caller may split them.
#define EXPOSURE_ADAPT_RATE 0.04f
// An exponential blend never quite arrives, so without a snap the resting value
// depends on the approach path -- which async texture-load timing perturbs, and
// which would make two equal-length headless runs differ. Snapping inside ~1%
// makes steady state a pure function of the scene, after which the adaptation
// holds no history at all.
//
// WHEN it settles depends on the scene, not on the deadband: the value has to
// stop moving too. On postfx_convergence_fixture it first touches the deadband
// around frame 18 and is pushed back out twice as the scene keeps brightening,
// settling at frame 91. An earlier note here said frame 12, measured before the
// percentiles changed what the meter reads and never re-taken.
#define EXPOSURE_ADAPT_SNAP 0.01f

void exposure_init(Exposure* ex) {
    if (!ex)
        return;
    memset(ex, 0, sizeof(Exposure));

    ex->multiplier = 1.0f;
    ex->bias_stops = 0.0f;
    ex->automatic = true;
    ex->key = 0.18f;

    // Aggressive at the bottom, and it has to be. A meter that includes the
    // black background is measuring the background: where no geometry was drawn
    // the buffer holds exactly 0, and zero times a thousand is still zero, so
    // that population never scales with the scene. Measured on cornell_point at
    // x1000, the raw metered value should move 9.966 stops and moves:
    //
    //   0.10 / 0.90   3.848 stops   (-6.118 error -- the meter reads background)
    //   0.50 / 0.95   9.832 stops   (-0.134)
    //   0.70 / 0.95   9.961 stops   (-0.005)
    //
    // Which is the same reason UE's Low Percent defaults to 80 rather than to
    // something mild. 0.50 measures nearly as straight but pins the gain at 1.0
    // on both test scenes -- accurate and inert, which is worse than useful.
    ex->meter_low = 0.70f;
    ex->meter_high = 0.95f;

    ex->meter_mode = METERING_UNIFORM;
    ex->meter_radius = 0.4f;

    // Genuinely inert: outside the histogram's own range at both ends, so a
    // scene has to author its way into being clamped. UE's Min/Max Brightness.
    //
    // They shipped at -13.3 / 19.93 for one review cycle, described as inert and
    // as stopping a runaway, and were neither. -13.3 sat INSIDE the bin range
    // and silently clamped any dark frame by up to thirteen stops. And the upper
    // bound never reached the gain at all: exposure_auto_gain floors the gain at
    // 2^-20, which pins once the metered value passes log2 17.53 -- below 19.93 --
    // so the bound moved the recorded luminance and left the exposure untouched.
    // The runaway it was credited with stopping was already bounded by that floor.
    ex->meter_min_log2 = -30.0f;
    ex->meter_max_log2 = 24.0f;

    ex->adapt_rate_up = EXPOSURE_ADAPT_RATE;
    ex->adapt_rate_down = EXPOSURE_ADAPT_RATE;

    // Off by default: every scene authored before the physical camera existed
    // expects the linear multiplier, and switching silently would rescale all
    // of them. The settings are a lit interior (EV100 ~6.9), which is what the
    // fixtures are, so a scene opting in starts somewhere sane.
    ex->physical = false;
    ex->aperture = 2.8f;
    ex->shutter_speed = 1.0f / 60.0f;
    ex->iso = 400.0f;
}

float exposure_ev100(const Exposure* ex) {
    if (!ex)
        return 0.0f;
    // Guarded because a zero shutter or ISO is a divide by zero that reaches the
    // shader as an INF exposure and blanks the frame -- a scene file can author
    // both.
    float n = fmaxf(ex->aperture, 1e-3f);
    float t = fmaxf(ex->shutter_speed, 1e-6f);
    float iso = fmaxf(ex->iso, 1.0f);
    return log2f((n * n) / t * 100.0f / iso);
}

float exposure_camera_multiplier(const Exposure* ex) {
    if (!ex)
        return 1.0f;
    if (!ex->physical)
        return ex->multiplier; // the linear value it always was
    // Saturation-based speed (Frostbite / ISO 12232): the luminance that maps to
    // white is 1.2 * 2^EV100, so exposure is its reciprocal. Positive stops open
    // up, matching exposure compensation on a real camera.
    float ev = exposure_ev100(ex) - ex->bias_stops;
    return 1.0f / (1.2f * exp2f(ev));
}

float exposure_auto_gain(const Exposure* ex) {
    if (!ex || !ex->automatic || !ex->adapted_valid)
        return 1.0f;
    if (!(ex->adapted_luminance > 0.0f))
        return 1.0f;
    // Capped at 1: auto-exposure only ever DARKENS an over-bright scene.
    // Brightening is what it must not do -- a subject framed against a black
    // void meters low and would blow out.
    //
    // THIS LINE IS NOW THE WHOLE GUARANTEE. It used to be belt-and-braces over a
    // metering floor pinned at the key, which made a sub-key mean impossible in
    // the first place; the comment here said it clamped "anyway, because that
    // invariant lives in a shader and this is the code depending on it". 11.52
    // deleted that floor -- it was an ABSOLUTE threshold and cost 1.61 stops of
    // scale-covariance -- so the shader can and does now report a mean below the
    // key, and nothing but this fminf stops it becoming a brightening gain.
    float gain = ex->key / ex->adapted_luminance;
    return fminf(fmaxf(gain, exp2f(-EXPOSURE_MAX_STOPS_DOWN)), 1.0f);
}

float exposure_multiplier(const Exposure* ex) {
    return exposure_camera_multiplier(ex) * exposure_auto_gain(ex);
}

void exposure_submit_measurement(Exposure* ex, float log2_luminance) {
    if (!ex)
        return;
    // isfinite rejects the NaN a degenerate frame can produce and the INF an
    // overflowed measure target can. Latching either would multiply into
    // preExposure and blank every frame after it, with nothing to recover from.
    if (!isfinite(log2_luminance))
        return;

    // Clamp before blending, not after: the bound is on what the scene is
    // allowed to have measured, so a pathological frame never enters the
    // history. Clamping the blended value instead would let it sit pinned at the
    // bound while the real measurement walked further away, and then crawl back
    // at the adaptation rate once the frame recovered.
    float target = fminf(fmaxf(log2_luminance, ex->meter_min_log2), ex->meter_max_log2);
    ex->last_raw_log2 = log2_luminance;
    ex->last_target_log2 = target;

    float adapted = target;
    if (ex->adapted_valid) {
        float prev = log2f(ex->adapted_luminance);
        // Which rate applies is decided by the direction the SCENE moved, in
        // luminance -- a target above `prev` is a brightening scene, which the
        // eye follows by stopping down. Naming them for the scene rather than
        // for the gain is what keeps "up" meaning the same thing here as it does
        // in the GUI.
        float rate = target > prev ? ex->adapt_rate_up : ex->adapt_rate_down;
        adapted = prev + (target - prev) * fminf(fmaxf(rate, 0.0f), 1.0f);
        if (fabsf(adapted - target) < EXPOSURE_ADAPT_SNAP)
            adapted = target;
    }

    float lum = exp2f(adapted);
    if (!isfinite(lum) || lum <= 0.0f)
        return;
    ex->adapted_luminance = lum;
    ex->adapted_valid = true;
}

void exposure_probe_report(const Exposure* ex, float raw_log2, int frame) {
    if (!ex)
        return;
    // Both halves separately as well as their product, because they fail
    // differently and the frame only shows the product: a camera multiplier is
    // free of scene content and identical across runs, while the gain is the
    // half that adapts and therefore the half that can differ between two builds
    // of the same scene.
    float camera = exposure_camera_multiplier(ex);
    float gain = exposure_auto_gain(ex);
    // adapted in LOG2 as well as nits, because a reader comparing it against
    // raw_log2 otherwise has to log2 a value already rounded to six decimals --
    // and the quantisation that introduces grows as the scene darkens, which is
    // exactly where a convergence check needs to be tightest.
    float adapted_log2 = ex->adapted_luminance > 0.0f ? log2f(ex->adapted_luminance) : 0.0f;
    printf("exposure-probe frame=%d raw_log2=%.6f raw_nits=%.6f target_log2=%.6f "
           "adapted_log2=%.6f adapted_nits=%.6f gain=%.6f camera=%.6f pre_exposure=%.6f "
           "ev100=%.6f key=%.6f valid=%d\n",
           frame, raw_log2, exp2f(raw_log2), ex->last_target_log2, adapted_log2,
           ex->adapted_luminance, gain, camera, camera * gain, exposure_ev100(ex), ex->key,
           ex->adapted_valid ? 1 : 0);
}

void exposure_reset_adaptation(Exposure* ex) {
    if (!ex)
        return;
    ex->adapted_luminance = 0.0f;
    ex->adapted_valid = false;
}
