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

struct Scene;
struct Trees;

// The lake itself: the water, calm and dark and held to its own basin, and what stands at its
// edge -- the ring that stops a wader, the dock out from the cabin's bank, reeds, a few drowned
// trees, and two rowboats, one moored at the dock and one turned over on the bank. The dead trees
// must still hold their models. `always_drawn` keeps the water drawn wherever the eye is, for a
// world with no fog to hide it switching off.
void lake_build(Kit* kit, struct Scene* scene, struct Trees* trees, unsigned int seed,
                bool always_drawn);

struct AudioSystem;

// The water lapping at the shore, heard from wherever along it is nearest the eye.
void lake_start_audio(struct AudioSystem* audio);

// Each frame: the water drawn only while the eye is in or near the valley, a wake where the
// feet wade, and the lapping moved along the shore to stay nearest the eye.
void lake_update(const struct Scene* scene, const float eye[3], const float feet[3]);

#endif // _SILENT_LAKE_H_
