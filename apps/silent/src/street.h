#ifndef _SILENT_STREET_H_
#define _SILENT_STREET_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "kit.h"
#include "layout.h"

// Everything outside: the road, its kerbs and sidewalks, our yards, the
// neighbours' houses, the lamps (and their light, at night), the poles and
// their wires, a car, fences -- and the fog that fills the street and stops at
// the house, unless not `fogged`. The far houses' front doors go to `far_doors`, a lot each from
// the west, for the terrace's stairs.
void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, bool fogged,
                  float far_doors[TERRACE_LOTS]);

// A street lamp standing at (x, y, z), its arm out along `yaw`'s +z, lit at night unless dead.
// Returns its light, or NULL when it has none.
Light* street_lamp(Kit* kit, Scene* scene, float x, float y, float z, float yaw, bool night,
                   bool dead, int profile);

// The street lamps' IES profile in the scene's library, or -1 by day or when it will not load.
int street_lamp_profile(Scene* scene, bool night);

#endif // _SILENT_STREET_H_
