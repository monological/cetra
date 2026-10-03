#ifndef _SILENT_KITCHEN_H_
#define _SILENT_KITCHEN_H_

#include "kit.h"

// Everything standing in the kitchen: the L of cabinets with the sink and the
// stove, the hood, the uppers, the fridge, the shelves, the table and its
// chairs, the window's frame, and the clutter, placed from `seed`.
void kitchen_build(Kit* kit, unsigned int seed);

// Where the fridge's motor is, in the world: what its hum comes from.
void kitchen_fridge_motor(vec3 out);

// Whether a point on the kitchen floor stands on the mat in front of the stove.
bool kitchen_on_mat(const vec3 p);

#endif // _SILENT_KITCHEN_H_
