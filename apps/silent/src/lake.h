#ifndef _SILENT_LAKE_H_
#define _SILENT_LAKE_H_

#include "cetra/game/audio.h"
#include "cetra/light.h"
#include "cetra/scene.h"
#include "cetra/water.h"

#include "kit.h"

/*
 * The lake valley (spec 13.41): the ground past the crossroads' south arm, falling to a lake
 * whose water stands at LAKE_Y, the track down to it, and the pad its cabin stands on.
 */

// The lake once built, held by its owner.
typedef struct Lake {
    Water* water;   // the scene's, NULL if it could not be made
    float reach;    // the water is on while the eye is this near it in plan; 0 = always
    Sound* lapping; // NULL until lake_start_audio
    WaterWake wake;
    // The dock lamp's chain and lantern, swinging about the hook under the crook's end, and the
    // light at its bulb, lamp_drop under the hook (spec 13.56).
    SceneNode* lantern;
    Light* lamp;
    const Scene* scene; // whose breeze swings it; none hangs it still
    vec3 lamp_pivot;
    float lamp_drop;
    float swing[2], swing_rate[2]; // radians leaning downwind and swaying across, and their rates
} Lake;

// `h`, the ground the rest of the world has at (x, z), with the valley carved into it: the
// ridge where the chasm turns, the bowl round the water and the bed under it, the track, and
// the slope up to the cabin's pad. `h` exactly wherever none of them reaches.
float lake_ground(float x, float z, float h);

// How far (x, z) is from the shoreline in plan: positive on land, negative over the water.
float lake_shore_distance(float x, float z);

// How far (x, z) is from the track's centre line.
float lake_track_distance(float x, float z);

// The shoreline on bearing `theta` from the lake's middle, and the track's centre line at `t` of
// its length, 0 on the cross street's asphalt and 1 on the cabin's pad.
void lake_shore_point(float theta, float* x, float* z);
void lake_track_point(float t, float* x, float* z);

// The track's surface from the cross street's asphalt to the cabin's pad, and the pad.
void lake_ground_build(Kit* kit);

struct Trees;

// The lake itself: the water, calm and dark and held to its own basin, its waves of the model
// `waves`, and what stands at its edge -- the ring that stops a wader, the dock out from the
// cabin's bank and its lamp, reeds, a few drowned trees, and the body round the boat turned over
// on the bank. The dead trees must still hold their models. The water is on only while the eye is
// within `reach` of it, 0 for always.
void lake_build(Lake* lake, Kit* kit, Scene* scene, struct Trees* trees, unsigned int seed,
                float reach, WaterWaveModel waves);

// Where the boat is kept (spec 13.56): its middle in plan and its heading (radians about +Y, the
// bow toward (sin, 0, cos)) moored off the dock's end, and the point on the dock's tall post its
// rope ties to when `post` is not NULL.
void lake_mooring(float* x, float* z, float* heading, vec3 post);
// Where its twin lies turned over on the bank: its middle in plan, the ground there, its heading.
void lake_bank_boat(float* x, float* z, float* ground, float* heading);
// How far (x, z) is out in the water a boat can take, past the bodies that stop a wader: negative
// short of it.
float lake_open_water(float x, float z);
// The dock in plan, the bodies down its sides and across its end included: min x, min z, max x,
// max z.
void lake_dock_box(float box[4]);

// The water lapping at the shore, on the cabin's bank until the eye moves it.
void lake_start_audio(Lake* lake, AudioSystem* audio);

// Each frame, from this frame's eye: the water on or off by its reach, a wake where the feet wade
// when they are `afoot` (not sat in the boat), the lapping moved to the shore on the eye's bearing
// from the lake's middle, and the dock lamp swung `dt` on by the breeze at `time`.
void lake_update(Lake* lake, const float eye[3], const float feet[3], bool afoot, double time,
                 float dt);

// The dock lamp hanging straight down while `rest`, for a capture that keeps what it sees, and
// where its swing has it after.
void lake_rest(Lake* lake, bool rest);

#endif // _SILENT_LAKE_H_
