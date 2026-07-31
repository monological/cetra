
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "exposure.h"
#include "ext/log.h"

Exposure* create_exposure(void) {
    Exposure* ex = malloc(sizeof(Exposure));
    if (!ex) {
        log_error("Failed to allocate memory for exposure");
        return NULL;
    }
    memset(ex, 0, sizeof(Exposure));

    ex->bias = 1.0f;
    ex->automatic = true;
    ex->key = 0.18f;

    // Off by default: every scene authored before the physical camera existed
    // expects `bias` to be a linear multiplier, and switching silently would
    // rescale all of them. The settings are a lit interior (EV100 ~6.9), which
    // is what the fixtures are, so a scene opting in starts somewhere sane.
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
        return ex->bias; // linear mode: the multiplier it always was
    // Saturation-based speed (Frostbite / ISO 12232): the luminance that maps to
    // white is 1.2 * 2^EV100, so exposure is its reciprocal. `bias` is a stops
    // offset here, not a multiplier -- positive opens up.
    float ev = exposure_ev100(ex) - ex->bias;
    return 1.0f / (1.2f * exp2f(ev));
}
