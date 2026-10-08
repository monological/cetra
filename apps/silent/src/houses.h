#ifndef _SILENT_HOUSES_H_
#define _SILENT_HOUSES_H_

#include <stdbool.h>

#include "kit.h"

/*
 * The street's houses, built in KitFrames with +X running along the facade and
 * +Z out of it toward the road.
 */

#define ROOF_PITCH 0.36f // a roof's rise, as a fraction of the house's depth
#define ROOF_THICK 0.1f  // a roof slope's, its fascia hanging this far under its edge
// Drops a second a metre of bare eave and of porch roof edge drips at the reference rain:
// far below the hundred and more a real eave sheds, which is a sheet rather than drips.
#define EAVE_DRIPS_PER_M  0.6f
#define PORCH_DRIPS_PER_M 1.0f

// A pitched roof over a box: two slopes and the two gable ends, the ridge
// running along the facade, overhanging by `overhang` all round. The frame's
// origin is a front corner at the ground, and the box fills a in [0, width]
// and d in [-depth, 0]. Returns the height of the roof's edge, the eave carried
// out over the overhang.
float house_gable_roof(Kit* kit, const KitFrame* f, float width, float depth, float eave_y,
                       float overhang, int mat_gable);

// Where a neighbour stands in plan: the middle of its front door where it meets the ground, and
// its walls' extent, the foundation included.
typedef struct HousePlot {
    vec3 door;
    float x0, x1, z0, z1;
} HousePlot;

// A neighbour: foundation, clad body, roof, chimney, porch and door, and a
// grid of windows, some lit. Solid and collidable; nobody goes in. The frame's
// origin is the middle of the lot's front edge, on its ground. Its bare front eave and its porch
// roof's edge drip. Where it stands goes to `plot`.
void house_neighbour(Kit* kit, const KitFrame* f, KitRng* rng, bool night, HousePlot* plot);

#endif // _SILENT_HOUSES_H_
