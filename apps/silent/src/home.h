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

// The living room's television's middle, west of the hall (negated): the sofa, the chair and the
// table face it.
#define TV_X 3.3f

/*
 * The basement under the whole house (spec 13.31): its slab's top, and the ground floor over it
 * -- finished boards on a subfloor on joists, the joists inside the 0.3 m the floor stands above
 * the yard, so the basement's ceiling is that floor's own framing seen from below.
 */
#define BASEMENT_Y   (-2.4f)
#define FLOOR_BOARDS 0.025f // the finished boards' thickness, under FLOOR_Y
#define SUBFLOOR_Y0  0.255f // the subfloor, up to the boards
#define JOIST_Y0     0.02f  // the joists' feet, up to the subfloor
// The stairwell down to it: behind the basement door, between the partition that closes it off
// from the stair up and the back wall, and west from the door's own threshold, where the flight
// starts.
#define STAIRWELL_WALL_Z 18.25f
#define STAIRWELL_Z0     (STAIRWELL_WALL_Z + 0.5f * INT_WALL)
#define CELLAR_HEAD_X    (-1.6f)
// Its door off the hall, and its window high in the back wall over the flight.
#define BASEMENT_DOOR_Z0 18.35f
#define BASEMENT_DOOR_Z1 19.15f
#define STAIR_WIN_X0     (-3.7f)
#define STAIR_WIN_X1     (-3.1f)
#define STAIR_WIN_SILL   (FLOOR_Y + 1.3f)
#define STAIR_WIN_HEAD   (FLOOR_Y + 2.0f)

// The walls, floors, rooms, furniture and lamps, and the roofs, porch and gutter; not the
// television, which is tv.c's.
void home_build(Kit* kit, Engine* engine, Scene* scene);

// The front door, the bathroom's and the basement's, hung to swing; false for one that could
// not be.
bool home_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                     PhysicsWorld* physics);
bool home_bath_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                    PhysicsWorld* physics);
bool home_basement_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                        PhysicsWorld* physics);

// How far `p` is outside the house's walls in plan; negative inside.
float home_outside_distance(const vec3 p);

#endif // _SILENT_HOME_H_
