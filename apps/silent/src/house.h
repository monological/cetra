#ifndef _SILENT_HOUSE_H_
#define _SILENT_HOUSE_H_

#include <cglm/cglm.h>

#include "kit.h"

// The player's house: two storeys of floors, ceilings and walls with their openings, the
// great hall's stair and gallery, the roofs, the porch, the gutter along the front with its
// downpipe, which leaks, and the tower. What stands in the rooms is kitchen.c's.
void house_build(Kit* kit);

// How far `p` is outside the house's walls in plan, tower included; negative inside.
float house_outside_distance(const vec3 p);

#endif // _SILENT_HOUSE_H_
