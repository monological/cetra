#ifndef _RAIN_H_
#define _RAIN_H_

#include <cglm/cglm.h>
#include <stdbool.h>

/*
 * Falling rain and what it leaves behind (spec 13.9).
 *
 * ONE PHYSICAL KNOB, and everything else is derived from it or scales what was
 * derived: `rate_mmh`, the rain rate a weather report gives. From it come the drop
 * sizes (Marshall-Palmer), how fast they fall (Atlas 1973), how much the air dims
 * (the geometric-optics extinction of that size distribution) and how quickly the
 * world gets wet. A knob that is only a look is a scale that defaults to 1, so the
 * physical answer is always what a scene gets before anyone tunes it.
 *
 * WORLD UNITS ARE METRES here. Every length and speed in this file is in metres and
 * metres per second, and a world authored at another scale is not supported rather
 * than silently converted.
 *
 * What this file owns is the RAIN, not how it is drawn: the rate, the derived
 * physics, and the state the rain accumulates over time -- how wet the world is and
 * how full its puddles are. That state is integrated exactly (a closed-form
 * exponential per regime), so it is the same after one step of 1/30 s as after two
 * of 1/60 s, and a headless run lands on the same value at any frame pacing.
 */

// The categories a rate falls into, in mm/h (American Meteorological Society).
#define RAIN_RATE_LIGHT    2.5f
#define RAIN_RATE_MODERATE 7.6f
#define RAIN_RATE_VIOLENT  50.0f

// The rate the wetting and filling time constants are quoted at. Heavier rain wets
// faster in proportion, since the film is supplied at the rate the rain falls.
#define RAIN_RATE_REFERENCE 5.0f

// Marshall-Palmer's intercept, drops per cubic metre per millimetre of diameter.
#define RAIN_MP_N0 8000.0f

typedef struct Rain {
    // ENGINE-OWNED: the rain's accumulated state and its clock. Read freely, never
    // write; rain_settle is how a caller asks for a world already soaked.
    float wetness;      // 0 = dry, 1 = every exposed surface carries a film
    float puddle_level; // 0 = no standing water, puddle_coverage = full at this rate
    float time;         // seconds of rain clock, the engine's render time

    // SETTINGS: plain stores. Write them directly, at any time.
    float rate_mmh; // rain rate in mm/h; 0 = no rain falls (the state still dries)
    vec3 wind;      // m/s; the horizontal air speed the drops are carried at

    // Time constants of the accumulated state, in seconds. Wetting and filling are
    // quoted at RAIN_RATE_REFERENCE and shorten in proportion to the rate; drying and
    // draining are evaporation and runoff, which the rain does not set.
    float wet_time;
    float dry_time;
    float puddle_fill_time;
    float puddle_drain_time;
    // The ceiling on the puddle level, reached only in heavy rain. 0 = no puddles.
    float puddle_coverage;

    // Width in metres of the square around the camera over which cover is known --
    // the occlusion map's footprint. Outside it every surface counts as open sky.
    float occlusion_extent;
} Rain;

// Created with a moderate rain's defaults at rate 0: nothing falls until a rate is set.
Rain* create_rain(void);
void free_rain(Rain* rain);

// Advance the accumulated state by `dt` seconds and latch the clock to `t`. NULL is a
// no-op, which is the no-rain scene.
void rain_update(Rain* rain, float t, float dt);

// Put the state where this rate settles it, as though it had been raining for ever --
// the scene that opens in a storm. At rate 0 that is dry.
void rain_settle(Rain* rain);

// Where the state is heading at this rate: 1 while any rain falls, 0 otherwise, and
// the puddle level the rate's inflow holds up against drainage.
float rain_wetness_target(const Rain* rain);
float rain_puddle_target(const Rain* rain);

// True while rain falls or anything it left is still wet: what decides whether cover
// has to be known this frame. NULL is false.
bool rain_active(const Rain* rain);

// The unit direction the rain travels: the median drop's fall speed carried sideways
// by the wind. Straight down when nothing falls.
void rain_fall_direction(const Rain* rain, vec3 out);

/*
 * The physics, as pure functions of the rate so a probe can print them for any rate
 * and a gate can hold them against the closed forms. `d_mm` is a drop DIAMETER in
 * millimetres.
 */
float rain_mp_lambda(float rate_mmh);                    // Marshall-Palmer slope, 1/mm
float rain_terminal_velocity(float d_mm);                // m/s, Atlas et al. 1973
float rain_extinction(float rate_mmh);                   // 1/m, geometric optics (Q_ext = 2)
float rain_drop_density(float rate_mmh, float d_min_mm); // drops/m^3 above d_min
float rain_median_diameter(float rate_mmh);              // mm, the volume-weighted median D0

// --rain-probe: the physics at a fixed ladder of rates, an integration schedule run
// twice at different pacing, and this rain's own rate and state. Needs no GL.
void rain_probe_print(const Rain* rain);

#endif // _RAIN_H_
