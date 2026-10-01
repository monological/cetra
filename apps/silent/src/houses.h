#ifndef _SILENT_HOUSES_H_
#define _SILENT_HOUSES_H_

#include <stdbool.h>

#include "kit.h"

/*
 * The street's houses, built in KitFrames with +X running along the facade and
 * +Z out of it toward the road.
 */

#define ROOF_PITCH 0.36f // a roof's rise, as a fraction of the house's depth

// A pitched roof over a box: two slopes and the two gable ends, the ridge
// running along the facade, overhanging by `overhang` all round. The frame's
// origin is a front corner at the ground, and the box fills a in [0, width]
// and d in [-depth, 0]. Returns the height of the roof's edge, the eave carried
// out over the overhang.
float house_gable_roof(Kit* kit, const KitFrame* f, float width, float depth, float eave_y,
                       float overhang, int mat_gable);

// A neighbour: foundation, clad body, roof, chimney, porch and door, and a
// grid of windows, some lit. Solid and collidable; nobody goes in. The frame's
// origin is the middle of the lot's front edge. Its bare front eave and its porch
// roof's edge drip.
void house_neighbour(Kit* kit, const KitFrame* f, KitRng* rng, bool night);

#endif // _SILENT_HOUSES_H_
