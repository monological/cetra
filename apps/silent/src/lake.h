#ifndef _SILENT_LAKE_H_
#define _SILENT_LAKE_H_

#include "kit.h"

/*
 * The lake valley (spec 13.41): the ground past the crossroads' south arm, falling to a lake
 * whose water stands at LAKE_Y, the track down to it, and the pad its cabin stands on.
 */

// Metres out from the shore the bed reaches wading depth, where a ring of bodies stops anyone
// going further.
#define LAKE_WADE_OUT 5.0f

// `h`, the ground the rest of the world has at (x, z), with the valley carved into it: the
// ridge where the chasm turns, the bowl round the water and the bed under it, the track, and
// the slope up to the cabin's pad. `h` exactly wherever none of them reaches.
float lake_ground(float x, float z, float h);

// How far (x, z) is from the shoreline in plan: positive on land, negative over the water.
float lake_shore_distance(float x, float z);

// How far (x, z) is from the track's centre line.
float lake_track_distance(float x, float z);

// The track's surface from the cross street's asphalt to the cabin's pad, and the pad.
void lake_ground_build(Kit* kit);

#endif // _SILENT_LAKE_H_
