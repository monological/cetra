#ifndef CETRA_SHORE_CHAIN_H
#define CETRA_SHORE_CHAIN_H

#include <stdbool.h>

#include "shore_runup.h"

struct Water;

/*
 * THE SWASH FILM (spec 11.45).
 *
 * A closed-form run-up says where the water's edge is; it cannot say that THIS wave ran into
 * the last one's backwash and stopped short, because that is a collision between two bodies of
 * water and a formula has no water in it. shoreRunup fakes the effect with a capture term --
 * scale the run-up down by how big the previous cycle was -- and that is a good imitation of an
 * average, not of an event.
 *
 * So: one Lagrangian chain per alongshore column of the traced shoreline. Nodes are parcels of
 * water running up the beach normal, carrying position and velocity, with each segment's
 * conserved rest volume giving its depth. Forces are shallow-water in PRESSURE form --
 * eta = terrain + volume/length, a = -g d(eta)/dx.
 *
 * A LINEAR SPRING LAW IS NOT USABLE HERE, and this is the trap worth recording: with a linearly
 * preloaded chain every uniform spacing balances the interior, so perturbations settle into
 * piled-up states rather than into a flat surface. The pressure form pins the equilibrium to
 * "surface flat" uniquely.
 *
 * The seaward node is driven by shoreRunup's own edge rather than by the wave field: it keeps
 * ONE forcing model across both wave paths, and the spectral cascades exist only on the GPU so
 * there is nothing on this side to sample them with.
 *
 * What leaves here is the TIP of each column -- how far up the beach the water reached -- kept
 * for the last few frames so a lit surface can ask what the beach remembers. That is a few
 * hundred floats, which is a uniform block rather than a texture, which is why this feature
 * costs zero sampler units in a shader that has none left.
 */

// Nodes per column. The film is centimetres thick over metres of beach, so the resolution that
// matters is along the run, and 32 puts a node every few centimetres of a typical swash.
#define SHORE_CHAIN_NODES 32
// Columns around the shore. Independent of the traced polyline's own point count -- the chain
// resamples it, so a 300-point shoreline and a 900-point one cost the same.
#define SHORE_CHAIN_COLS 64
/*
 * Tip history depth, in SLOTS -- and a slot is one tap of the shader's drying march, not one
 * frame of the sim.
 *
 * Those are different rates and conflating them was a real defect: recording per frame gave a
 * fifth of a second of history where the taps reach back over SHORE_TAP_PERIODS of wave time,
 * so every tap but the first ran off the end of the ring and the wet sand collapsed to two
 * tones. The sim still steps every frame; it just writes a slot on the tap interval, which is
 * what shore_runup_slot_interval returns. One spare over SHORE_TAPS so the oldest tap
 * interpolates between two real slots rather than clamping.
 */
#define SHORE_CHAIN_HISTORY (SHORE_TAPS + 3)
_Static_assert(SHORE_CHAIN_HISTORY > SHORE_TAPS,
               "the ring must outspan the taps or the oldest ones clamp to one slot");

typedef struct ShoreChain {
    // Node position along the beach normal, world units, measured from the still waterline
    // (negative seaward). Row-major: column-major runs of SHORE_CHAIN_NODES.
    float x[SHORE_CHAIN_COLS * SHORE_CHAIN_NODES];
    float u[SHORE_CHAIN_COLS * SHORE_CHAIN_NODES]; // node velocity, units/s
    float vol[SHORE_CHAIN_NODES - 1];              // conserved rest volume per segment

    // Ring buffer of tip positions: [slot][column]. Slot `head` is the newest.
    float tips[SHORE_CHAIN_HISTORY][SHORE_CHAIN_COLS];
    int head;
    // Time since the last slot was written. The sim runs per frame and the ring advances on
    // the tap interval, so this is what carries the difference between the two rates.
    float slot_clock;
    // Steps since the seed. The film needs a few seconds before it means anything: a push at
    // the handover travels at sqrt(g h), which is seconds to cross a beach, so anything read
    // before then is the rest state with a ripple starting across it.
    int steps;

    // Each column's world frame, resampled from the shoreline polyline.
    float origin[SHORE_CHAIN_COLS * 2]; // world x,z of the waterline point
    float normal[SHORE_CHAIN_COLS * 2]; // unit landward normal
    bool wraps;                         // the shoreline closed, so columns are a ring

    float rest_span; // how far up the beach the chain reaches at rest, world units
    float seaward_x; // where the handover node rests, world units (negative = seaward)
    bool ready;      // geometry built and state seeded
} ShoreChain;

/*
 * Rebuild the column frames from the water's traced shoreline.
 *
 * Two costs, and they are not the same: the RESAMPLE of the polyline into columns runs every
 * call (a re-baked bed can move the points without moving the span, so there is nothing cheaper
 * to test), and the RESEED of the node state is what returns immediately unless the shoreline's
 * reach or its closure actually changed. The resample is one pass over the polyline, not one
 * per column.
 */
void shore_chain_rebuild(ShoreChain* chain, const struct Water* water,
                         const ShoreRunupParams* params);

// Advance one frame. `dt` is the render delta; the step is substepped internally for CFL.
void shore_chain_step(ShoreChain* chain, const ShoreRunupParams* params, float t, float dt);

#endif // CETRA_SHORE_CHAIN_H
