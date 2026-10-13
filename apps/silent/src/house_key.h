#ifndef _SILENT_HOUSE_KEY_H_
#define _SILENT_HOUSE_KEY_H_

#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The front door's key (spec 13.53): a heavy brass bit key lying on the basement's workbench, the
 * one key in the house. Taking it puts it in the backpack.
 */

typedef struct HouseKey {
    SceneNode* node; // lying on the workbench, and kept off the graph once it is taken
    vec3 at;         // its middle, which the player reaches for
} HouseKey;

// The key on the workbench.
void house_key_build(HouseKey* key, Engine* engine, Scene* scene);
void house_key_free(HouseKey* key);

// The key alone, centred, along +d and standing on its edge, its bit up, so a picture from the
// side is its face.
void house_key_model(Kit* kit);

#endif // _SILENT_HOUSE_KEY_H_
