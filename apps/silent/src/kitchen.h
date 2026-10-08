#ifndef _SILENT_KITCHEN_H_
#define _SILENT_KITCHEN_H_

#include "cetra/game/audio.h"

#include "kit.h"

// Everything standing in the kitchen: the L of cabinets with the sink and the
// stove, the hood, the uppers, the fridge, the shelves, the table and its
// chairs, the window's frame, and the clutter, placed from `seed`.
void kitchen_build(Kit* kit, unsigned int seed);

// The fridge's hum, started at its motor; nothing when `audio` is NULL.
void kitchen_start_audio(AudioSystem* audio);

// Whether a point on the kitchen floor stands on the mat in front of the stove.
bool kitchen_on_mat(const vec3 p);

// A board of preserves in frame `f` from a0 to a1 along it at height y: two staggered rows out
// from the wall, the taller jars at the back, with the odd gap where one has been taken.
void kitchen_preserves(Kit* kit, const KitFrame* f, KitRng* rng, float a0, float a1, float y);

// A ladder-back chair standing on frame `f`'s origin, its sitter facing its +d.
void kitchen_chair(Kit* kit, const KitFrame* f);

#endif // _SILENT_KITCHEN_H_
