#ifndef _SILENT_CABIN_H_
#define _SILENT_CABIN_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/fire.h"
#include "cetra/scene.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"

#include "door.h"
#include "kit.h"

/*
 * The log cabin on the lake's east bank (spec 13.41), facing the water: round logs over a stone
 * footing under a rusted tin roof, a fieldstone chimney up its back, and a porch toward the dock.
 * Inside, one room, as somebody left it: the fire still burning, a meal laid for one with the
 * lamp beside it lit, the cot, the cold stove, the shelves and a map pinned up.
 */

// Everything that stands still, into `kit` in the world's coordinates, the lamp's wick with it.
void cabin_build(Kit* kit, unsigned int seed);

// The fire in the hearth, burning from load, and the light across the firebox's mouth it drives,
// cached since it never moves.
void cabin_light(FireSystem* fs, Scene* scene, bool shadows);

struct AudioSystem;
// The hearth's crackle and the room's own quiet, each where it is.
void cabin_start_audio(struct AudioSystem* audio);

// The door, hung on its south jamb against the room's face of the wall, so it swings in.
bool cabin_door(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics);

#endif // _SILENT_CABIN_H_
