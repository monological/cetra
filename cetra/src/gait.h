#ifndef _GAIT_H_
#define _GAIT_H_

/*
 * A walker's gait, as its eye and its ears know it (spec 13.47): where in its stride a body is,
 * when a foot comes down, how its eye rides the ground, and how fast a person keeps climbing. For
 * a first-person walker with no body to animate.
 *
 * THE STRIDE RUNS ON GROUND COVERED, not on time or on the speed asked for, so a walker held
 * against a wall stops walking, and a frame rate cannot change how many steps a path takes. On
 * level ground a step is `step_base + step_per_speed * speed` long, which puts a walk of 1.6 m/s
 * at 120 steps a minute and a jog of 3.4 at 163, as people walk and jog. On a flight it is
 * Blondel's rule: one step for every `flight_step` of rise counted twice plus run, which is a step
 * a tread on any stair built to the rule, whatever the stair collides as.
 *
 * TWO HALVES, AT TWO RATES, so nothing the simulation reads depends on how frames fall:
 *  - gait_step, once a fixed step with that step's exact move: the grade of the ground, how much
 *    of it is a flight, the pace a person keeps on it, and time in the air.
 *  - gait_frame, once a rendered frame with the frame's move and its time: the stride, the
 *    footfalls, and where the eye is put.
 * Both take DISPLACEMENTS, never positions, so neither cares where the world's origin is.
 * gait_reset is for a body that is put somewhere rather than walked there.
 *
 * THE EYE LAGS THE GROUND, it does not follow it rigidly. A character controller climbs a kerb in
 * one step and drops off one in another, and an eye riding it rigidly jumps with it; this eye
 * trails the body's height on a critically damped spring and catches up, never further below
 * than `lag_below` nor above than `lag_above` -- the second smaller, so a low ceiling over a
 * flight going down is never put through. In the air it catches up at once, near enough.
 *
 * THE HEAD RISES AND FALLS OVER THE FEET, and is moved, never turned. It is lowest as a heel
 * strikes, highest over the standing foot, and swayed toward that foot once a stride. A body's
 * centre rises and falls 4 to 5 cm walking and 6 to 8 jogging, and sways 2 to 4 cm; the defaults
 * are about half, since the eyes turn against the head's motion to hold what they look at, and a
 * camera carried at a body's full excursion reads as far more than a person feels. Nothing pitches
 * or rolls the view, which is what makes a bob sickening: a move alone leaves the far distance
 * still and only the near ground swims. It fades out on a flight, whose climb is its own rise.
 *
 * NO GL, NO PHYSICS, NO CLOCK and no game type, as camera_rig.h, so an arm can drive it with no
 * window at all.
 */

#include <cglm/cglm.h>
#include <stdbool.h>

typedef enum GaitFallKind {
    GAIT_FALL_STEP,    // a heel striking, on level ground or a flight
    GAIT_FALL_SETTLE,  // the foot in the air set down by the other when the body stops
    GAIT_FALL_SHUFFLE, // a foot moved round while turning on the spot
    GAIT_FALL_LAND,    // both feet, coming down out of the air
} GaitFallKind;

typedef struct GaitFootfall {
    GaitFallKind kind;
    int foot;    // 0 the left, 1 the right; a landing is both and says 0
    float speed; // the ground speed it was taken at, m/s; for a landing, how fast it came down
} GaitFootfall;

typedef struct Gait {
    // ENGINE-OWNED: what it worked out. Read freely, never write.
    // gait_step's:
    float grade;  // rise over run of the ground walked lately, signed: up is positive
    float stairs; // 0..1, how much that ground is a flight rather than a slope
    float pace;   // 0..1, the share of the speed asked that a person keeps on it
    // gait_frame's:
    float phase;           // steps taken: a whole number has a foot under the body, x.5 a heel down
    float speed;           // ground speed, smoothed, m/s
    vec3 smooth_offset;    // where the eye goes from where the body puts it: its lag on the
                           // ground's height, and a landing's dip
    vec3 bob_offset;       // and where the head goes over the feet as they walk
    bool fell;             // a foot came down this frame
    GaitFootfall footfall; // which, while `fell`
    // The two halves' own state.
    float rise, run;  // the grade's sums, each weighted down by the ground walked since
    float air;        // seconds off the ground, by the steps
    float fall;       // how fast the body was coming down at the last step in the air, m/s
    float landing;    // how fast a landing came down, for the frame to take; 0 when none
    float lag, lag_v; // the eye's lag on the body's height (m, negative below) and its rate
    float dip, dip_v; // a landing's dip and its rate
    float turned;     // radians turned standing since the last shuffle step
    float since;      // seconds since the last footfall
    float yaw;        // the body's heading at the last frame
    bool yawed;       // `yaw` holds one
    bool moving;      // walking, by the smoothed speed
    int shuffles;     // shuffle steps taken, which foot is next

    // SETTINGS: plain stores, written directly at any time. Last in the struct, which is what
    // gait_reset keeps.
    float step_base, step_per_speed; // a level step, m: base + per_speed * speed
    float flight_step;               // a step on a flight, as twice the rise plus the run, m
    float stairs_from, stairs_to;    // the grades over which ground turns from slope to flight
    float grade_reach;               // metres of ground the grade is taken over
    float climb_up, climb_down;      // a person's rate of climb, as a share of the speed asked
    float pace_floor;                // the slowest a climb brings a walker to, m/s
    float speed_time;                // seconds the ground speed is smoothed over
    float stop_speed;                // under this the body is standing, m/s
    float eye_rate;                  // rad/s of the eye's spring on the ground
    float eye_air_rate;              // and in the air
    float lag_below, lag_above;      // the furthest the eye trails below, and stays above, m
    float land_air;                  // seconds in the air before coming down is a landing
    float dip_per_speed;             // a landing's dip, m per m/s it came down at
    float dip_max;                   // m
    float dip_rate;                  // rad/s of the dip's spring
    float shuffle_turn;              // radians turned standing for a shuffle step
    float shuffle_gap;               // the fewest seconds between two
    float bob_per_speed;             // the head's rise and fall, top to bottom, m per m/s
    float bob_max;                   // and the most it comes to, m
    float sway;                      // its sway from one foot to the other, side to side, m
    float sway_speed;                // the speed the sway is all there by, m/s
    float bob_scale; // the player's: 1 as authored, 0 no bob at all, exactly as though none
} Gait;

// A gait standing still, with the settings above at the values they are argued for in gait.c.
Gait* create_gait(void);
void free_gait(Gait* g);

// Forgets the ground and the stride, keeping the settings: for a body put somewhere.
void gait_reset(Gait* g);

// A fixed step's exact move and whether it ended on the ground, the speed the walker asked for
// before `pace` scaled it, and the step's length in seconds.
void gait_step(Gait* g, const vec3 move, bool grounded, float speed_asked, float dt);

// A rendered frame's move -- the body as drawn this frame less as drawn the last -- whether the
// last step ended on the ground, the body's heading (radians about +Y, facing (sin, 0, cos)) and
// the frame's seconds. A frame of no time changes nothing and lets no foot fall.
void gait_frame(Gait* g, const vec3 move, bool grounded, float yaw, float dt);

#endif // _GAIT_H_
