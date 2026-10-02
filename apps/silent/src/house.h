#ifndef _SILENT_HOUSE_H_
#define _SILENT_HOUSE_H_

#include <cglm/cglm.h>

#include "door.h"
#include "kit.h"

// The player's house: two storeys of floors, ceilings and walls with their openings, the
// great hall's stair and gallery, the roofs, the porch, the gutter along the front with its
// downpipe, which leaks, and the tower. What stands in the rooms is kitchen.c's.
void house_build(Kit* kit);

// The house's walls that rooms are dressed against, each as it is built: interior.c lays its
// panelling and its stone on them, round the same openings.
typedef enum {
    HOUSE_WALL_FRONT,
    HOUSE_WALL_BACK,
    HOUSE_WALL_WEST,
    HOUSE_WALL_EAST,
    HOUSE_WALL_HALL_EAST,
    HOUSE_WALL_HALL_WEST,
    HOUSE_WALL_UP_EAST,
    HOUSE_WALL_UP_WEST,
    HOUSE_WALL_GREAT_FRONT,
    HOUSE_WALL_COUNT
} HouseWall;

const KitWall* house_wall(HouseWall which);

// The doors that open, hung in their openings into `doors`, at most `max`; returns how many.
int house_doors(Door* doors, int max, Engine* engine, Scene* scene, EntityManager* em,
                PhysicsWorld* physics);

// The main roof's top over x: its ridge runs front to back on x = 0, and the slopes fall to
// the side walls. Its underside is ROOF_THICK below.
float house_roof_y(float x);

// How far `p` is outside the house's walls in plan, tower included; negative inside.
float house_outside_distance(const vec3 p);

#endif // _SILENT_HOUSE_H_
