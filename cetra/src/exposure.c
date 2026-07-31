
#include <math.h>
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

Exposure* create_exposure(void) {
    Exposure* ex = malloc(sizeof(Exposure));
    if (!ex) {
        log_error("Failed to allocate memory for exposure");
        return NULL;
    }
    memset(ex, 0, sizeof(Exposure));

    ex->multiplier = 1.0f;
    ex->bias_stops = 0.0f;
    ex->automatic = true;
    ex->key = 0.18f;

    // Off by default: every scene authored before the physical camera existed
    // expects the linear multiplier, and switching silently would rescale all
    // of them. The settings are a lit interior (EV100 ~6.9), which is what the
    // fixtures are, so a scene opting in starts somewhere sane.
    ex->physical = false;
    ex->aperture = 2.8f;
    ex->shutter_speed = 1.0f / 60.0f;
    ex->iso = 400.0f;

    return ex;
}

void free_exposure(Exposure* ex) {
    free(ex);
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
    // Capped at 1: auto-exposure only ever DARKENS an over-bright scene. The
    // metering floor equals the key, so the measured mean cannot fall below it
    // and this cannot exceed 1 -- but clamp anyway, because that invariant lives
    // in a shader and this is the code depending on it. Brightening is what it
    // must not do: a subject framed against a black void meters low and would
    // blow out.
    float gain = ex->key / ex->adapted_luminance;
    return fminf(fmaxf(gain, exp2f(-EXPOSURE_MAX_STOPS_DOWN)), 1.0f);
}

float exposure_multiplier(const Exposure* ex) {
    return exposure_camera_multiplier(ex) * exposure_auto_gain(ex);
}

void exposure_set_adapted_luminance(Exposure* ex, float luminance) {
    if (!ex)
        return;
    // isfinite rejects the NaN a degenerate frame can produce and the INF an
    // overflowed measure target can. Latching either would multiply into
    // preExposure and blank every frame after it, with nothing to recover from.
    if (!isfinite(luminance) || luminance <= 0.0f)
        return;
    ex->adapted_luminance = luminance;
    ex->adapted_valid = true;
}

void exposure_reset_adaptation(Exposure* ex) {
    if (!ex)
        return;
    ex->adapted_luminance = 0.0f;
    ex->adapted_valid = false;
}
