#ifndef _SILENT_HOME_H_
#define _SILENT_HOME_H_

#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "door.h"
#include "kit.h"
#include "layout.h"

/*
 * The player's house (spec 13.25), on the lot at the plan's origin: two storeys of clapboard on
 * layout.h's footprint, its front door, hall, kitchen and clock where layout.h has them. The
 * ground floor is the hall, as in P.T., running straight from the front door to a window at the
 * back, with a cased opening half way; the kitchen and the living room off its front half, the
 * bathroom off its back; and the stair, the back bedroom and the basement shut. Nobody goes
 * upstairs.
 */

// The cross walls behind the living room and the bathroom, and the bathroom's east wall.
#define HOME_SPLIT_Z 16.6f
#define BATH_X1      2.6f

// The doorways off the hall: the living room's, open, and the bathroom's, whose door opens.
#define LIVING_DOOR_Z0 11.6f
#define LIVING_DOOR_Z1 12.5f
#define BATH_DOOR_Z0   14.4f
#define BATH_DOOR_Z1   15.2f

// The bathroom's and the living room's inner faces.
#define BATH_IN_X0   (HALL_X1 + 0.5f * INT_WALL)
#define BATH_IN_X1   (BATH_X1 - 0.5f * INT_WALL)
#define BATH_IN_Z0   (KITCHEN_BACK_Z + 0.5f * INT_WALL)
#define BATH_IN_Z1   (HOME_SPLIT_Z - 0.5f * INT_WALL)
#define LIVING_IN_X0 (HOUSE_X0 + 0.5f * EXT_WALL)
#define LIVING_IN_X1 (HALL_X0 - 0.5f * INT_WALL)
#define LIVING_IN_Z0 BAND_Z0
#define LIVING_IN_Z1 (HOME_SPLIT_Z - 0.5f * INT_WALL)
// And the hall's, front to back.
#define HALL_IN_Z0 BAND_Z0
#define HALL_IN_Z1 (HOUSE_BACK_Z - 0.5f * EXT_WALL)

// The walls, floors, rooms, furniture and lamps, and the roofs, porch and gutter.
void home_build(Kit* kit, Engine* engine, Scene* scene);

// The front door and the bathroom's, hung to swing; false for one that could not be.
bool home_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                     PhysicsWorld* physics);
bool home_bath_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                    PhysicsWorld* physics);

// How far `p` is outside the house's walls in plan; negative inside.
float home_outside_distance(const vec3 p);

#endif // _SILENT_HOME_H_
