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
    // ENGINE-OWNED: the rain's accumulated state. Read freely, never write; rain_settle
    // is how a caller asks for a world already soaked.
    float wetness;      // 0 = dry, 1 = every exposed surface carries a film
    float puddle_level; // 0 = no standing water, puddle_coverage = full at this rate

    // SETTINGS: plain stores. Write them directly, at any time.
    float rate_mmh; // rain rate in mm/h; 0 = no rain falls (the state still dries)
    vec3 wind;      // m/s; the horizontal air speed the drops are carried at
    // Scale on how fast drops fall; 1 = their terminal velocity. Below 1 is gentler rain on
    // screen -- slower and shorter streaks -- at the same rate, and it tilts the cover with
    // the fall so what shelters is still what the drops miss. A look, not a measurement.
    float fall_scale;

    // Time constants of the accumulated state, in seconds. Wetting and filling are
    // quoted at RAIN_RATE_REFERENCE and shorten in proportion to the rate; drying and
    // draining are evaporation and runoff, which the rain does not set.
    float wet_time;
    float dry_time;
    float puddle_fill_time;
    float puddle_drain_time;
    // The ceiling on the puddle level, reached only in heavy rain. 0 = no puddles.
    float puddle_coverage;
    float puddle_scale; // metres across a typical puddle
    // The rings drops leave in standing water: how far each tilts the surface (1 = the
    // default tilt, 0 = still water) and how far apart they land, in metres.
    float ripple_strength;
    float ripple_size;
    // Scale on how much wetting darkens a surface; 1 = physical, where soaked rough concrete
    // falls to about a quarter of its dry brightness. The film's shine is untouched. A look.
    float wet_darkening;

    // Width in metres of the square around the camera over which cover is known --
    // the occlusion map's footprint. Outside it every surface counts as open sky.
    float occlusion_extent;
    // Metres over which a surface goes from dry under cover to wet in the open: the soft
    // edge of the dry patch under an eave, where wind and splash carry the rain in a little.
    // Capped at a texel of the map (extent / 1024).
    float occlusion_softness;

    // The streaks: `streak_count` drops in each of three nested boxes around the camera,
    // the innermost `streak_radius` metres either way and each next one three times
    // wider, so rain is dense at hand and thins with distance as perspective shrinks it.
    int streak_count;
    float streak_radius;
    float shutter_s;         // the exposure a streak is smeared over, seconds
    float streak_width;      // scale on each drop's own diameter as drawn; 1 = physical
    float streak_brightness; // scale on each drop's opacity; 1 = physical
    float streak_forward_g;  // Henyey-Greenstein asymmetry of the refracted lobe
    float streak_glint;      // 0..1, how much of the lamp-lit light arrives as flashes
    // How much brighter than what it refracts a drop reads: 0 = physical, where rain in the
    // dark shows only where light hits it; the default 2 = three times its surroundings, so
    // it shows everywhere there is anything to catch. A look, not a measurement.
    float streak_sheen;

    // The splashes: `splash_count` slots on a grid `splash_radius` metres either way of the
    // camera, each throwing the droplets of the drops big enough to splash that land in its
    // cell, as often as the rate lands them there. 0 = none.
    int splash_count;
    float splash_radius;
    float splash_amount; // scale on how many drops splash; 1 = physical
    float splash_size;   // scale on the droplets' diameter; 1 = physical

    // Scale on the rain's extinction as a medium past the streaks; 1 = physical, 0 = none.
    float mist;
} Rain;

// Created with a moderate rain's defaults at rate 0: nothing falls until a rate is set.
Rain* create_rain(void);
void free_rain(Rain* rain);
// The same defaults written over a Rain the caller holds, dry.
void rain_init_defaults(Rain* rain);

// Advance the accumulated state by `dt` seconds. NULL is a no-op, which is the no-rain scene.
void rain_update(Rain* rain, float dt);

// Put the state where this rate settles it, as though it had been raining for ever --
// the scene that opens in a storm. At rate 0 that is dry.
void rain_settle(Rain* rain);

// Where the state is heading at this rate: 1 while any rain falls, 0 otherwise, and
// the puddle level the rate's inflow holds up against drainage.
float rain_wetness_target(const Rain* rain);
float rain_puddle_target(const Rain* rain);

// True while rain falls: a positive rate. NULL is false.
bool rain_falling(const Rain* rain);

// True while rain falls or anything it left is still wet: what decides whether cover
// has to be known this frame. NULL is false.
bool rain_active(const Rain* rain);

// The fraction of the ripple cells a drop lands in, 0..1: none when nothing falls, all of
// them from moderate rain up.
float rain_ripple_activity(const Rain* rain);

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
float rain_splash_flux(float rate_mmh); // drops over RAIN_SPLASH_MIN_MM landing, 1/(m^2 s)

// --rain-probe: the physics at a fixed ladder of rates, an integration schedule run
// twice at different pacing, and this rain's own rate and state. Needs no GL.
void rain_probe_print(const Rain* rain);

#endif // _RAIN_H_
