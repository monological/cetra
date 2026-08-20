#ifndef _EROSION_H_
#define _EROSION_H_

#include <stdbool.h>

#include "terrain.h"

// Hydraulic and thermal erosion over a TerrainField, as a load-time bake.
//
// This is the reason terrain grew a stored grid at all. Erosion cannot be written
// as f(x, z): the height at a point depends on how much water flowed THROUGH it,
// which depends on the whole upstream watershed, so there is no closed form to
// evaluate and the field has to exist before the sim can run over it.
//
// Eulerian (a grid of cells) rather than Lagrangian (a population of droplets),
// and that is a determinism requirement rather than a preference. Every stage
// below is double-buffered so an output cell reads only the previous state, which
// makes disjoint row bands independent and the result bitwise identical at any
// thread count -- the same property the cloud noise bake relies on. Droplets write
// to whatever cell they land in, so two of them in one cell resolve by arrival
// order and no two thread counts agree.
//
// The masks are not a side effect; they are half the point. A sim knows where
// water ran, so it can say where gravel and silt belong. Deriving that from slope
// and altitude cannot, which is why terrain shaded that way reads as procedural
// however carefully the palette is chosen.

typedef struct ErosionParams {
    int iterations; // fixed, never a convergence test -- a tolerance would make
                    // the result depend on float ordering and lose determinism

    float dt;
    float rain;        // water added per cell per second
    float evaporation; // fraction of standing water lost per second

    // Sediment capacity is capacity * sin(tilt) * |velocity| * depth. dissolve and
    // deposit are the rates at which the difference between capacity and the load
    // is taken from or returned to the ground.
    float capacity;
    float dissolve;
    float deposit;
    // Floor under sin(tilt). At exactly zero a flat cell has zero capacity, so it
    // drops its whole load in one step and stamps a ring wherever water slows.
    float min_tilt;

    // Thermal (dry) erosion: material above the talus slope slides downhill. This
    // is what puts scree under a cliff and stops hydraulic-only terrain from
    // keeping walls no real slope holds.
    float talus;        // tangent of the angle of repose
    float thermal_rate; // fraction of the excess moved per pass
    int thermal_every;  // run one thermal pass every N hydraulic iterations

    // 0 = size it from the machine. A parameter of the BAKE and not of the
    // physics, and it is here so a caller can pin it: the claim this whole design
    // rests on is that the number of workers does not reach the output, and a
    // claim nothing outside the process can vary is a claim nothing can test.
    int workers;
} ErosionParams;

// What the bake did, for --terrain-erosion-probe. A frame cannot check any of
// this: eroded terrain with the sediment budget silently wrong still looks like
// eroded terrain.
typedef struct ErosionStats {
    double height_before; // summed ground height before and after. The hydraulic
    double height_after;  // stages only MOVE material, so these must agree once
    double sediment_left; // what is still suspended is added back
    double eroded_total;
    double deposited_total;

    // Peaks before the masks are normalised. Zero here means the sim ran and did
    // nothing, which normalisation would otherwise hide by scaling noise to 1.
    float flow_peak;
    float deposit_peak;
    float wear_peak;

    // How the work was split. Reported because it is the one input to the bake a
    // caller did not choose, and the whole determinism claim is that it does not
    // reach the output. There is deliberately no TIMING here: this module owns no
    // clock, so the caller measures the call with the one it already has.
    int workers;

    // FNV-1a over the raw bytes of all four planes, in index order.
    //
    // This is the determinism claim reduced to one printable number, and it exists
    // because the SUMS above cannot make it: addition hides compensating
    // differences, so two thread counts that disagree cell by cell can still
    // report identical totals to every digit. Two runs agreeing here agree bit for
    // bit. Note it covers the masks as well as the height -- a sim that erodes
    // identically while accumulating its masks in a racy order is still wrong.
    unsigned long long digest;
} ErosionStats;

ErosionParams erosion_default_params(void);

// Erodes field->height in place and fills field's three masks, each normalised to
// its own peak. `terrain` supplies extent only -- the cell size the sim needs is
// 2*extent/(res-1), and a sim run at the wrong world scale is stable but wrong.
// stats may be NULL. False leaves the field untouched.
bool terrain_erode(TerrainField* field, const TerrainParams* terrain, const ErosionParams* params,
                   ErosionStats* stats);

#endif // _EROSION_H_
