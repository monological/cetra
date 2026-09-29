#ifndef _SILENT_STREET_H_
#define _SILENT_STREET_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "kit.h"

// Everything outside: the road, its kerbs and sidewalks, the yards, the
// neighbours' houses, the lamps (and their light, at night), the poles and
// their wires, a car, fences -- and the fog that fills the street and stops at
// the house. Call after the flashlight exists: the fog beams the scene's FIRST
// spot, and that has to be the player's, not a lamp's.
void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night);

#endif // _SILENT_STREET_H_
