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
 * `procedural/water_waves.c` is the precedent and also the warning: it is the CPU twin of the
 * Gerstner half and CLAUDE.md records that "nothing compares the two, which is the known gap".
 * This one ships with the comparison that one lacks -- `--shore-probe` prints what this
 * computes, and a gate arm reads it against what the shader put on screen. If the twin drifts,
 * something says so.
 *
 * The constants live in shore.glsl and are mirrored in the .c beside their GLSL names, so a
 * grep for any one of them finds both copies.
 */

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

#endif // CETRA_SHORE_RUNUP_H
