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
 * bathroom off its back; the stair and the back bedroom shut; and at the hall's end a door onto
 * the cellar stair, down to a basement under the whole house (basement.h, spec 13.31). Nobody
 * goes upstairs.
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
#define LIVING_IN_X1 HALL_OUT_X0
#define LIVING_IN_Z0 BAND_Z0
#define LIVING_IN_Z1 (HOME_SPLIT_Z - 0.5f * INT_WALL)
// And the hall's, front to back.
#define HALL_IN_Z0 BAND_Z0
#define HALL_IN_Z1 (HOUSE_BACK_Z - 0.5f * EXT_WALL)

// The living room's television's middle, west of the hall (negated): the sofa, the chair and the
// table face it.
#define TV_X 3.3f

// A room's finish laid over a wall's face: paper, paint, tiles.
#define LINING 0.006f

/*
 * The basement under the whole house (spec 13.31): its slab's top, and the ground floor over it
 * -- finished boards on a subfloor on joists, the joists inside the 0.3 m the floor stands above
 * the yard, so the basement's ceiling is that floor's own framing seen from below.
 */
#define BASEMENT_Y   (-2.4f)
#define FLOOR_BOARDS 0.025f // the finished boards' thickness, under FLOOR_Y
#define SUBFLOOR_T   0.02f  // the subfloor's, under the boards
#define SUBFLOOR_Y0  (FLOOR_Y - FLOOR_BOARDS - SUBFLOOR_T)
#define JOIST_Y0     0.02f // the joists' feet, up to the subfloor
// Its walls' inner faces, which are the house's outside walls' carried down,
#define CELLAR_X0 (HOUSE_X0 + 0.5f * EXT_WALL)
#define CELLAR_X1 (HOUSE_X1 - 0.5f * EXT_WALL)
#define CELLAR_Z0 (HOUSE_FRONT_Z + 0.5f * EXT_WALL)
#define CELLAR_Z1 (HOUSE_BACK_Z - 0.5f * EXT_WALL)
// and their outer ones, which is where the yard is cut away round it.
#define DIG_X0 HOUSE_OUT_X0
#define DIG_X1 HOUSE_OUT_X1
#define DIG_Z0 HOUSE_OUT_Z0
#define DIG_Z1 HOUSE_OUT_Z1
// The stairwell down to it: behind the basement door, between the partition that closes it off
// from the stair up and the back wall, and west from the door's own threshold, where the flight
// starts.
#define STAIRWELL_WALL_Z 18.25f
#define STAIRWELL_Z0     (STAIRWELL_WALL_Z + 0.5f * INT_WALL)
#define CELLAR_HEAD_X    (-1.6f)

// The walls, floors, rooms, furniture and lamps, and the roofs, porch and gutter; not the
// television, which is tv.c's.
void home_build(Kit* kit, Engine* engine, Scene* scene);

// The stairwell's four walls inside, from the basement's floor to the ceiling, under one coat
// of `mat` cut round their openings -- the back wall's window, the basement door -- and round
// `foot` in the partition's: the way into the basement at the bottom of the flight.
void home_line_stairwell(Kit* kit, int mat, const KitOpening* foot);

// The front door, the bathroom's and the basement's, hung to swing; false for one that could
// not be.
bool home_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                     PhysicsWorld* physics);
bool home_bath_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                    PhysicsWorld* physics);
bool home_basement_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                        PhysicsWorld* physics);

#endif // _SILENT_HOME_H_
