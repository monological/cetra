#ifndef _SILENT_HOUSES_H_
#define _SILENT_HOUSES_H_

#include <stdbool.h>

#include "kit.h"

/*
 * The street's houses, built in a KitFrame whose origin is one front corner at
 * the ground, with +X running along the facade and +Z out of it toward the road.
 * A house fills a in [0, width] and d in [-depth, 0].
 */

// A pitched roof over a box: two slopes and the two gable ends, the ridge
// running along the facade, overhanging by `overhang` all round.
void house_gable_roof(Kit* kit, const KitFrame* f, float width, float depth, float eave_y,
                      float rise, float overhang, int mat_gable);

// A neighbour: foundation, clad body, roof, chimney, porch and door, and a
// grid of windows, some lit. Solid and collidable; nobody goes in.
void house_neighbour(Kit* kit, const KitFrame* f, KitRng* rng, bool night);

#endif // _SILENT_HOUSES_H_
