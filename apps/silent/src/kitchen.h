#ifndef _SILENT_KITCHEN_H_
#define _SILENT_KITCHEN_H_

#include "cetra/game/audio.h"

#include "cards.h"
#include "kit.h"

// Everything standing in the kitchen: the L of cabinets with the sink and the
// stove, the hood, the uppers, the fridge, the shelves, the table and its
// chairs, the window's frame, and the clutter, placed from `seed`.
void kitchen_build(Kit* kit, unsigned int seed);

// The fridge's hum, started at its motor; nothing when `audio` is NULL.
void kitchen_start_audio(AudioSystem* audio);

// Where the town map hangs on the freezer door (spec 13.43): the frame's origin is the middle of
// the map's top edge on the door's face, its d out of the door into the room.
void kitchen_fridge_map_frame(KitFrame* out);

// Whether a point on the kitchen floor stands on the mat in front of the stove.
bool kitchen_on_mat(const vec3 p);

// A board of preserves in frame `f` from a0 to a1 along it at height y: two staggered rows out
// from the wall, the taller jars at the back, with the odd gap where one has been taken.
void kitchen_preserves(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1, float y);

// A ladder-back chair standing on frame `f`'s origin, its sitter facing its +d.
void kitchen_chair(Kit* kit, const KitFrame* f);

// A stack of `count` white plates, and a mug with its handle toward +a, standing at (a, d) on
// height y in frame `f`.
void kitchen_plates(Kit* kit, const KitFrame* f, float a, float d, float y, int count);
void kitchen_mug(Kit* kit, const KitFrame* f, float a, float d, float y);

// A card pinned upright facing out of its wall at d, centred at (a, y) and turned `tilt` radians
// in its own plane.
void kitchen_pin_card(Kit* kit, const KitFrame* f, CardId card, float a, float y, float d,
                      float tilt);

#endif // _SILENT_KITCHEN_H_
