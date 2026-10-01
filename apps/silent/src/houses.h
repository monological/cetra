#ifndef _SILENT_HOUSES_H_
#define _SILENT_HOUSES_H_

#include <stdbool.h>

#include "cetra/rain.h"
#include "kit.h"

/*
 * The street's houses, built in KitFrames with +X running along the facade and
 * +Z out of it toward the road.
 */

#define ROOF_PITCH 0.36f // a roof's rise, as a fraction of the house's depth

// The lines the street drips from in the rain (spec 13.12), gathered as it is built and
// handed to the rain once there is one. Counted past the rain's cap, so the rain can say
// how many it dropped.
typedef struct Drips {
    RainDripLine lines[RAIN_DRIP_MAX];
    int count;
} Drips;

// A drip line from (a, y, d) to (a, y, d) in frame `f`, `rate` drops a second at the
// reference rain, landing at world Y `ground`. NULL `drips` collects nothing.
void drips_add(Drips* drips, const KitFrame* f, const vec3 from, const vec3 to, float rate,
               float ground);

// A pitched roof over a box: two slopes and the two gable ends, the ridge
// running along the facade, overhanging by `overhang` all round. The frame's
// origin is a front corner at the ground, and the box fills a in [0, width]
// and d in [-depth, 0].
void house_gable_roof(Kit* kit, const KitFrame* f, float width, float depth, float eave_y,
                      float overhang, int mat_gable);
// The height of that roof's edge, the eave carried out over the overhang; its fascia hangs
// HOUSE_ROOF_THICK under it.
float house_eave_tip_y(float eave_y, float overhang);
#define HOUSE_ROOF_THICK 0.1f

// A neighbour: foundation, clad body, roof, chimney, porch and door, and a
// grid of windows, some lit. Solid and collidable; nobody goes in. The frame's
// origin is the middle of the lot's front edge. Its bare front eave and its porch
// roof's edge drip.
void house_neighbour(Kit* kit, const KitFrame* f, KitRng* rng, bool night, Drips* drips);

#endif // _SILENT_HOUSES_H_
