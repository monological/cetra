#ifndef _SILENT_MANSION_H_
#define _SILENT_MANSION_H_

#include <stdbool.h>

#include <cglm/cglm.h>

#include "kit.h"
#include "layout.h"

/*
 * The mansion's front row (spec 13.25), in the house's own coordinates: the dining room in the
 * kitchen's footprint and a suit of armour in the hall where the player's house has its clock.
 * What the cat stands on there is stated here, since its places are measured off it.
 */

// The refectory table's middle, and the candelabra on it.
#define TABLE_X 2.45f
#define TABLE_Z 12.05f

// The window seat under the dining room's window: along the front wall from SEAT_X0 to
// SEAT_X1, out to its front edge at SEAT_Z1, its cushion's top SEAT_TOP above the floor.
#define SEAT_X0  1.35f
#define SEAT_X1  3.85f
#define SEAT_Z1  10.45f
#define SEAT_TOP 0.47f

// The chair at the table's west end, pushed in, facing the candelabra; its seat's top above
// the floor.
#define HEAD_CHAIR_X    1.0f
#define DINING_SEAT_TOP 0.47f

// The dining room, the armour and the candle stand by it, built into `kit`.
void mansion_front_build(Kit* kit);

// The middle candle's flame on the candelabra, in the world.
void mansion_candelabra_flame(vec3 out);

// Whether a point of the house's plan is on the dining room's rug or the window seat's
// cushion, where a paw makes no sound.
bool mansion_on_cloth(const vec3 p);

#endif // _SILENT_MANSION_H_
