#ifndef _SILENT_BACKPACK_H_
#define _SILENT_BACKPACK_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

/*
 * The player's backpack (spec 13.40): an old canvas daypack lying on the bed in the home's
 * bedroom, with a flashlight in it. Taking it takes what is in it, and the flashlight works from
 * then on. Each thing in it is a model of its own on no scene graph, which the backpack's screen
 * turns on its own.
 */

typedef enum { ITEM_FLASHLIGHT, ITEM_COUNT } ItemId;

typedef struct ItemSpec {
    const char* name; // as the backpack's screen lists it
    const char* line; // what the player knows about it
} ItemSpec;

extern const ItemSpec ITEMS[ITEM_COUNT];

typedef struct Backpack {
    SceneNode* bag; // lying on the bed until it is taken, then NULL
    vec3 at;        // the middle of it, which the player reaches for
    bool taken;
    SceneNode* models[ITEM_COUNT]; // each thing in it alone, for the backpack's screen
} Backpack;

// The bag on the bed, or nothing there when it is already `taken`, and the models of what is in
// it.
void backpack_build(Backpack* bp, Engine* engine, Scene* scene, bool taken);
// How far the eye is from the bag when it is in reach -- door_reach_distance's test -- and
// FLT_MAX when it is not, or is taken.
float backpack_reach_distance(const Backpack* bp, const vec3 eye, const vec3 forward, float reach,
                              float cone);
// Off the bed: the player has it, and what is in it.
void backpack_take(Backpack* bp);
// Whether the player has `item`.
bool backpack_holds(const Backpack* bp, ItemId item);
void backpack_free(Backpack* bp);

#endif // _SILENT_BACKPACK_H_
