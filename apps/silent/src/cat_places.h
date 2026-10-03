#ifndef _SILENT_CAT_PLACES_H_
#define _SILENT_CAT_PLACES_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "cetra/nav_graph.h"

#include "cetra/game/physics.h"

/*
 * The house as the places its cat goes between (spec 13.17), measured off layout.h and the
 * rooms' furniture: where it rests, where it passes, and the walks, the flight and the jumps
 * that join them.
 */

// The kinds of link, as the graph numbers them. A RAIL link is walked along the gallery's hand
// rail, one way, and only by a cat that set out to walk it: a route for anything else leaves
// it out.
enum { CAT_LINK_WALK, CAT_LINK_STAIR, CAT_LINK_JUMP, CAT_LINK_RAIL, CAT_LINK_KINDS };

// The places, by id: node i of the graph is CAT_PLACES[i].
typedef enum CatPlaceId {
    CAT_AT_STUDY_CHAIR,
    CAT_AT_CHAIR_JUMP,
    CAT_AT_STUDY_BAY,
    CAT_AT_STUDY_WINDOW,
    CAT_AT_STUDY_BAY_W,
    CAT_AT_STUDY_MID,
    CAT_AT_STUDY_DOOR,
    CAT_AT_GAL_STUDY,
    CAT_AT_GAL_W,
    CAT_AT_GAL_1,
    CAT_AT_GALLERY,
    CAT_AT_GAL_2,
    CAT_AT_GAL_3,
    CAT_AT_GAL_4,
    CAT_AT_GAL_E,
    CAT_AT_RAIL_W,
    CAT_AT_RAIL_1,
    CAT_AT_RAIL_2,
    CAT_AT_RAIL_3,
    CAT_AT_RAIL_E,
    CAT_AT_NEWEL_CAP,
    CAT_AT_STAIR_HEAD,
    CAT_AT_STAIR_TOP,
    CAT_AT_STAIR_BOTTOM,
    CAT_AT_STAIR_FOOT,
    CAT_AT_GREAT_STAIR,
    CAT_AT_GREAT_E,
    CAT_AT_GREAT_W,
    CAT_AT_GREAT_S,
    CAT_AT_RUG,
    CAT_AT_HEARTH,
    CAT_AT_ARCH,
    CAT_AT_HALL_CLOCK,
    CAT_AT_HALL_FRONT,
    CAT_AT_KITCHEN_DOOR,
    CAT_AT_KITCHEN_MID,
    CAT_AT_STOVE_MAT,
    CAT_AT_COUNTER_JUMP,
    CAT_AT_KITCHEN_WINDOW,
    CAT_AT_KCHAIR_JUMP,
    CAT_AT_KITCHEN_CHAIR,
    CAT_PLACE_COUNT
} CatPlaceId;

// How the cat rests at a place, if it stops there at all.
typedef enum { CAT_REST_NONE, CAT_REST_CURL, CAT_REST_LIE, CAT_REST_SIT } CatRest;

// A place's tag: somewhere too narrow to turn round on, so every turn there is made in the air.
#define CAT_TAG_BEAM 1u

typedef struct CatPlace {
    const char* name;
    vec3 feet;    // where its feet are standing there
    float radius; // the clear ground round it, which a corner is rounded inside
    CatRest rest;
    float yaw;     // which way it faces resting there, radians about +y, 0 facing +z
    uint32_t tags; // CAT_TAG_*
} CatPlace;

extern const CatPlace CAT_PLACES[CAT_PLACE_COUNT];

// A place's id by name, or -1.
int cat_place_find(const char* name);

// The graph of the house's places and links.
NavGraph* cat_places_build(void);

// What a route may use: everything but the rail, or everything.
NavQuery cat_places_query(bool rail);

// Every link against the world's static colliders, a hand above the cat's feet; each one
// blocked is logged by name. Returns how many.
int cat_places_check(const NavGraph* graph, PhysicsWorld* physics);

#endif // _SILENT_CAT_PLACES_H_
