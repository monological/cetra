#ifndef CETRA_SHORE_RUNUP_H
#define CETRA_SHORE_RUNUP_H

/*
 * THE CPU TWIN of shore.glsl's shoreRunup (spec 11.45).
 *
 * A deliberate duplicate, and the only one. The swash film's chain runs on the CPU and has to
 * be driven by the same run-up the shader draws, so the arithmetic exists twice -- there is no
 * way to share a closed form between C and GLSL short of generating one from the other, and
 * this is 40 lines.
 *
 * WHAT IS SHARED, AND WHAT IS NOT. The CONSTANTS are not duplicated: both sides include
 * shaders/include/shore_constants.glsl, which is written for both preprocessors. That is the
 * half that actually drifts -- a tuned number is what gets changed -- so it was worth the odd
 * include path to remove the possibility. The ARITHMETIC is duplicated and cannot be shared;
 * it is reviewable by eye in a way a bare literal is not.
 *
 * `procedural/water_waves.c` is the precedent and the warning: it is the CPU twin of the
 * Gerstner half and CLAUDE.md records that "nothing compares the two, which is the known gap".
 *
 * `--shore-probe` prints what this computes and the `shore-twin` gate arm reads it, but be
 * exact about what that arm establishes: it checks this copy is bounded by its own ceiling,
 * spreads across it, and does not repeat a period later. Every number in it comes from HERE.
 * It would not notice the shader's arithmetic changing, and it is not claimed to -- what rules
 * out the likeliest divergence is the shared constants above, not the arm.
 */

// The numbers, shared verbatim with the shader. See that file for why it is a .glsl included
// into C: it has to live where the shader preprocessor can resolve it and where a change to it
// retriggers the shader bake.
#include "../shaders/include/shore_constants.glsl"

typedef struct ShoreRunupParams {
    float surf_height;     // significant wave height at the shore, METRES
    float surf_omega;      // peak angular frequency, rad/s
    float beach_slope;     // mean foreshore slope, rise per run
    float units_per_metre; // world units in a metre
    float wind_dir[2];     // unit, the longest wave's travel direction
} ShoreRunupParams;

/*
 * The swash tongue's edge at a world position and time, in WORLD UNITS above the still level.
 * Matches ShoreRunup.edge in shore.glsl; the derivative is not mirrored because the chain
 * needs only the value.
 */
float shore_runup_edge(const ShoreRunupParams* p, float x, float z, float t);

// The highest the edge can ever reach, world units. Mirrors shoreEdgeCeiling().
float shore_runup_ceiling(const ShoreRunupParams* p);

// The beach slope with its floor applied. Mirrors shoreSlope(), and exported for the same
// reason that one exists: three call sites in the solver and the uniform publish had each
// re-spelled the floor as a bare literal, so the travel time, the chain and the shader could
// end up standing on different numbers.
float shore_runup_slope(const ShoreRunupParams* p);

// Seconds of wave time between history slots, which is the tap spacing shoreSwash marches at.
// The solver records at this rate rather than per frame so its ring spans the window the taps
// can ask about; see shore_constants.glsl.
float shore_runup_slot_interval(const ShoreRunupParams* p);

#endif // CETRA_SHORE_RUNUP_H
