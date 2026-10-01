#include "rain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ext/log.h"

// The Atlas fit's coefficients, which the streak shader draws every drop's fall with.
#include "../shaders/include/rain_constants.glsl"

Rain* create_rain(void) {
    Rain* rain = malloc(sizeof(Rain));
    if (!rain) {
        log_error("Failed to allocate Rain");
        return NULL;
    }
    rain_init_defaults(rain);
    return rain;
}

void rain_init_defaults(Rain* rain) {
    *rain = (Rain){0};
    // Game time, not weather time: a real pavement takes minutes to wet and most of an
    // hour to dry, and a scene that waited that long would never be seen to change. The
    // ORDER is what is kept -- a film comes and goes faster than a puddle does, and
    // anything dries far more slowly than it wets.
    rain->wet_time = 20.0f;
    rain->dry_time = 240.0f;
    rain->puddle_fill_time = 90.0f;
    rain->puddle_drain_time = 900.0f;
    rain->puddle_coverage = 0.6f;
    rain->puddle_scale = 3.0f;
    rain->ripple_strength = 1.0f;
    rain->ripple_size = 0.35f;
    rain->fall_scale = 1.0f;
    rain->wet_darkening = 1.0f;
    // A street's length either way of the player: past that the fog has taken most of
    // the frame, and a 1024-texel map spent over it is 9.4 cm a texel -- under the
    // overhang of an eave.
    rain->occlusion_extent = 96.0f;
    // A texel of the map at the default extent: the widest the cover's taps may spread.
    rain->occlusion_softness = 0.1f;
    // Real rain is about 900 drops a cubic metre at 10 mm/h; this draws some 30 in the
    // nearest box, and the rest of it is the medium's to carry (the fog, phase 7). The
    // shutter is a camera's 1/60 s, which streaks a 2 mm drop over 11 cm.
    rain->streak_count = 16384;
    rain->streak_radius = 4.0f;
    rain->shutter_s = 1.0f / 60.0f;
    rain->streak_width = 1.0f;
    rain->streak_brightness = 1.0f;
    rain->streak_forward_g = 0.8f;
    // The lamp's image in a drop is a point, so nearly all of what it sends arrives as the
    // flashes; the rest is smoothed by the drop's own blur and the lens's. A judgement, not
    // a measurement: the energy is the same at any value.
    rain->streak_glint = 0.8f;
    // Rain that reads everywhere, not only where a lamp lights it: the physical answer (0)
    // leaves a night scene's rain invisible outside its lamps, which is true and is not what
    // a scene asking for rain wants.
    rain->streak_sheen = 2.0f;
    // Moderate rain lands some 900 splashing drops a square metre a second, each throwing
    // droplets for a few tenths of one: far more than any budget draws. A slot splashes once a
    // life and stands for the rest, like a streak, over the ground a few steps round the eye.
    rain->splash_count = 4096;
    rain->splash_radius = 6.0f;
    rain->splash_amount = 1.0f;
    rain->splash_size = 1.0f;
    rain->mist = 1.0f;
}

void free_rain(Rain* rain) {
    free(rain);
}

bool rain_falling(const Rain* rain) {
    return rain && rain->rate_mmh > 0.0f;
}

float rain_wetness_target(const Rain* rain) {
    return rain_falling(rain) ? 1.0f : 0.0f;
}

// The level a rate's inflow holds up against drainage. The exponential is a SHAPE
// choice rather than a hydrology result: it rises in proportion to the rate for a
// drizzle, which barely pools, and saturates in a downpour, which pools everything
// the ground can hold. Its scale is the reference rate, so moderate rain stands at
// about two thirds of the ceiling.
float rain_puddle_target(const Rain* rain) {
    if (!rain_falling(rain))
        return 0.0f;
    return rain->puddle_coverage * (1.0f - expf(-rain->rate_mmh / RAIN_RATE_REFERENCE));
}

// The state decays exponentially and never reaches zero, so "still wet" has a floor:
// below a thousandth of a film nothing it drives is visible. The puddle level is not
// asked: a puddle is drawn in proportion to the wetness, and drains far more slowly.
#define RAIN_WET_FLOOR 1e-3f

bool rain_active(const Rain* rain) {
    return rain_falling(rain) || (rain && rain->wetness > RAIN_WET_FLOOR);
}

// In proportion to the rate up to the moderate band's ceiling. Most of what lands in a puddle
// is too small to ring visibly, so this counts the drops that do rather than all of them --
// a shape choice, and the reason a drizzle rings sparsely.
float rain_ripple_activity(const Rain* rain) {
    if (!rain_falling(rain))
        return 0.0f;
    return fminf(rain->rate_mmh / RAIN_RATE_MODERATE, 1.0f);
}

void rain_fall_direction(const Rain* rain, vec3 out) {
    glm_vec3_copy((vec3){0.0f, -1.0f, 0.0f}, out);
    if (!rain_falling(rain))
        return;
    // Clamped as the streaks' copy is, so the cover tilts with the drops that are drawn.
    const float fall = rain_terminal_velocity(rain_median_diameter(rain->rate_mmh)) *
                       fmaxf(rain->fall_scale, 0.0f);
    vec3 v = {rain->wind[0], rain->wind[1] - fall, rain->wind[2]};
    if (glm_vec3_norm(v) > 1e-6f)
        glm_vec3_normalize_to(v, out);
}

// One exact step of a first-order approach to `target`: the closed form, so the
// answer depends on the time elapsed and not on how it was sliced -- provided the
// regime (rising or falling, and the rate) holds across the slices.
static float _approach(float x, float target, float tau_up, float tau_down, float dt) {
    float tau = target > x ? tau_up : tau_down;
    if (!(tau > 0.0f))
        return target;
    return target + (x - target) * expf(-dt / tau);
}

// A rising time constant quoted at the reference rate, shortened in proportion to the
// rate. Only reached with a positive rate: a target above the state needs rain.
static float _rate_scaled(float tau, float rate_mmh) {
    return rate_mmh > 0.0f ? tau * RAIN_RATE_REFERENCE / rate_mmh : tau;
}

void rain_update(Rain* rain, float dt) {
    if (!rain || !(dt > 0.0f))
        return;
    rain->wetness = _approach(rain->wetness, rain_wetness_target(rain),
                              _rate_scaled(rain->wet_time, rain->rate_mmh), rain->dry_time, dt);
    rain->puddle_level = _approach(rain->puddle_level, rain_puddle_target(rain),
                                   _rate_scaled(rain->puddle_fill_time, rain->rate_mmh),
                                   rain->puddle_drain_time, dt);
}

void rain_settle(Rain* rain) {
    if (!rain)
        return;
    rain->wetness = rain_wetness_target(rain);
    rain->puddle_level = rain_puddle_target(rain);
}

float rain_mp_lambda(float rate_mmh) {
    return rate_mmh > 0.0f ? 4.1f * powf(rate_mmh, -0.21f) : INFINITY;
}

float rain_terminal_velocity(float d_mm) {
    return fmaxf(0.0f, RAIN_ATLAS_A - RAIN_ATLAS_B * expf(-RAIN_ATLAS_C * d_mm));
}

// Integral of Q_ext * pi D^2 / 4 over N0 exp(-Lambda D), with Q_ext = 2 for drops this
// much larger than the wavelength: pi N0 / Lambda^3. N0 is per mm of diameter and D^2
// is in mm^2, so the answer is in mm^2 per m^3 and 1e-6 carries it to 1/m.
float rain_extinction(float rate_mmh) {
    if (!(rate_mmh > 0.0f))
        return 0.0f;
    float lambda = rain_mp_lambda(rate_mmh);
    return (float)M_PI * RAIN_MP_N0 / (lambda * lambda * lambda) * 1e-6f;
}

float rain_drop_density(float rate_mmh, float d_min_mm) {
    if (!(rate_mmh > 0.0f))
        return 0.0f;
    float lambda = rain_mp_lambda(rate_mmh);
    return RAIN_MP_N0 / lambda * expf(-lambda * d_min_mm);
}

// The median of D^3 exp(-Lambda D), a gamma(4) in Lambda D, whose median is 3.672.
float rain_median_diameter(float rate_mmh) {
    return rate_mmh > 0.0f ? 3.672f / rain_mp_lambda(rate_mmh) : 0.0f;
}

// The number that land: the drops' density times their fall speed, integrated over the sizes
// that splash. With Atlas's A - B exp(-C D) that is closed form,
// N0 (A exp(-Lambda Dmin) / Lambda - B exp(-(Lambda + C) Dmin) / (Lambda + C)),
// and the units come out per square metre per second: N0 per m^3 per mm times mm times m/s.
float rain_splash_flux(float rate_mmh) {
    if (!(rate_mmh > 0.0f))
        return 0.0f;
    float lambda = rain_mp_lambda(rate_mmh);
    float d = RAIN_SPLASH_MIN_MM;
    return RAIN_MP_N0 *
           (RAIN_ATLAS_A * expf(-lambda * d) / lambda -
            RAIN_ATLAS_B * expf(-(lambda + RAIN_ATLAS_C) * d) / (lambda + RAIN_ATLAS_C));
}

/*
 * The same wet-then-dry schedule at two frame rates. The state is integrated in closed
 * form per regime, so the two must agree to float rounding; a forward-Euler step, or a
 * time constant read at the wrong rate, separates them.
 */
static void _probe_schedule(int fps) {
    Rain* r = create_rain();
    if (!r)
        return;
    const float dt = 1.0f / (float)fps;
    r->rate_mmh = 10.0f;
    for (int i = 0; i < fps * 10; i++)
        rain_update(r, dt);
    const float wet_mid = r->wetness, puddle_mid = r->puddle_level;
    r->rate_mmh = 0.0f;
    for (int i = 0; i < fps * 30; i++)
        rain_update(r, dt);
    printf("rain-probe schedule fps=%d rate=10 rain_s=10 dry_s=30 wet_mid=%.9g puddle_mid=%.9g "
           "wet_end=%.9g puddle_end=%.9g\n",
           fps, (double)wet_mid, (double)puddle_mid, (double)r->wetness, (double)r->puddle_level);
    free_rain(r);
}

void rain_probe_print(const Rain* rain) {
    const float rates[] = {1.0f, RAIN_RATE_LIGHT, RAIN_RATE_MODERATE, 10.0f, RAIN_RATE_VIOLENT};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        const float r = rates[i];
        const float d0 = rain_median_diameter(r);
        printf("rain-probe physics rate=%.9g lambda=%.9g beta=%.9g density=%.9g d0=%.9g "
               "v0=%.9g splash=%.9g\n",
               (double)r, (double)rain_mp_lambda(r), (double)rain_extinction(r),
               (double)rain_drop_density(r, 0.5f), (double)d0, (double)rain_terminal_velocity(d0),
               (double)rain_splash_flux(r));
    }
    const float diameters[] = {0.5f, 1.0f, 2.0f, 4.0f};
    for (size_t i = 0; i < sizeof(diameters) / sizeof(diameters[0]); i++)
        printf("rain-probe velocity d=%.9g v=%.9g\n", (double)diameters[i],
               (double)rain_terminal_velocity(diameters[i]));

    Rain* defaults = create_rain();
    if (defaults) {
        printf("rain-probe defaults wet_time=%.9g dry_time=%.9g fill_time=%.9g drain_time=%.9g "
               "coverage=%.9g reference=%.9g\n",
               (double)defaults->wet_time, (double)defaults->dry_time,
               (double)defaults->puddle_fill_time, (double)defaults->puddle_drain_time,
               (double)defaults->puddle_coverage, (double)RAIN_RATE_REFERENCE);
        free_rain(defaults);
    }
    _probe_schedule(60);
    _probe_schedule(30);

    if (!rain) {
        printf("rain-probe state present=0\n");
        return;
    }
    printf("rain-probe state present=1 rate=%.9g wetness=%.9g puddle=%.9g "
           "wet_time=%.9g dry_time=%.9g fill_time=%.9g drain_time=%.9g coverage=%.9g "
           "streak_count=%d streak_radius=%.9g mist=%.9g\n",
           (double)rain->rate_mmh, (double)rain->wetness, (double)rain->puddle_level,
           (double)rain->wet_time, (double)rain->dry_time, (double)rain->puddle_fill_time,
           (double)rain->puddle_drain_time, (double)rain->puddle_coverage, rain->streak_count,
           (double)rain->streak_radius, (double)rain->mist);
}
