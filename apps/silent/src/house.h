#ifndef _SILENT_HOUSE_H_
#define _SILENT_HOUSE_H_

#include <cglm/cglm.h>

#include "door.h"
#include "kit.h"

// The Gothic house, which stands at the end of the street as the mansion (spec 13.25): two
// storeys of floors, ceilings and walls with their openings, the great hall's stair and
// gallery, the roofs, the porch, the gutter along the front with its downpipe, which leaks, and
// the tower. Built in the house's own plan; the kit stands it where it goes.
void house_build(Kit* kit);

// The house's walls that rooms are dressed against, each as it is built, so what is laid on
// one -- panelling, a stone lining -- goes round the same openings.
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

// The openings named outside the wall table, by their index in their wall's.
enum {
    OPENING_FRONT_DOOR = 0,    // HOUSE_WALL_FRONT
    OPENING_DINING_WINDOW = 1, // HOUSE_WALL_FRONT
    OPENING_DINING_DOOR = 0,   // HOUSE_WALL_HALL_EAST
    OPENING_PARLOUR_DOOR = 0,  // HOUSE_WALL_HALL_WEST
    OPENING_GREAT_ARCH = 0,    // HOUSE_WALL_GREAT_FRONT: the hall's arch,
    OPENING_STUDY_DOOR = 1,    // the study's doorway, open,
    OPENING_BOX_ROOM_DOOR = 2, // and the two shut off the gallery
    OPENING_BEDROOM_DOOR = 3,
    OPENING_STAIR_LANCET = 0, // HOUSE_WALL_EAST: over the stair
};

const KitWall* house_wall(HouseWall which);

// The one door that opens, the front door, hung in its opening of a house standing at `origin`;
// false if it could not be.
bool house_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                      PhysicsWorld* physics, const vec3 origin);

// The gutter both houses hang under the pent strip's edge, which is `edge_y` high at z =
// `fascia`, from x0 to x1, with its downpipe at the east end and its leaks as drip lines.
void house_front_gutter(Kit* kit, float x0, float x1, float edge_y, float fascia);

// The main roof's top over x, and its underside: its ridge runs front to back on x = 0, and
// the slopes fall to the side walls.
float house_roof_y(float x);
float house_roof_under_y(float x);

// How far `p` is from the nearest face of any wall of the house or the tower, in plan, or of
// any floor or ceiling, in height, each taken whole -- openings, ends and storeys ignored, so it
// errs toward too near. Negative inside one. What an irradiance probe must keep clear of.
float house_clearance(const vec3 p);

#endif // _SILENT_HOUSE_H_
