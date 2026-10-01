#ifndef _SILENT_STREET_H_
#define _SILENT_STREET_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "houses.h"
#include "kit.h"

// Everything outside: the road, its kerbs and sidewalks, the yards, the
// neighbours' houses, the lamps (and their light, at night), the poles and
// their wires, a car, fences -- and the fog that fills the street and stops at
// the house. What drips in the rain goes on `drips`.
void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, Drips* drips);

#endif // _SILENT_STREET_H_
