#ifndef _SILENT_CAT_PLACES_H_
#define _SILENT_CAT_PLACES_H_

#include <cglm/cglm.h>

#include "cetra/nav_graph.h"

#include "cetra/game/physics.h"

/*
 * The house as the places its cat goes between (spec 13.17), measured off layout.h and the
 * rooms' furniture: where it rests, where it passes, and the walks, the flight and the jumps
 * that join them.
 */

// The kinds of link, as the graph numbers them.
enum { CAT_LINK_WALK, CAT_LINK_STAIR, CAT_LINK_JUMP, CAT_LINK_KINDS };

// How the cat rests at a place, if it stops there at all.
typedef enum { CAT_REST_NONE, CAT_REST_CURL, CAT_REST_LIE, CAT_REST_SIT } CatRest;

typedef struct CatPlace {
    const char* name;
    vec3 feet;    // where its feet are standing there
    float radius; // the clear ground round it, which a corner is rounded inside
    CatRest rest;
    float yaw; // which way it faces resting there, radians about +y, 0 facing +z
} CatPlace;

// Node i of the graph is CAT_PLACES[i].
extern const CatPlace CAT_PLACES[];
extern const int CAT_PLACE_COUNT;

// The graph of the house's places and links.
NavGraph* cat_places_build(void);

// Every link against the world's static colliders, a hand above the cat's feet; each one
// blocked is logged by name. Returns how many.
int cat_places_check(const NavGraph* graph, PhysicsWorld* physics);

#endif // _SILENT_CAT_PLACES_H_
