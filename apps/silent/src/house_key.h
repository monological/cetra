#ifndef _SILENT_HOUSE_KEY_H_
#define _SILENT_HOUSE_KEY_H_

#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The front door's key (spec 13.53): a heavy brass bit key, the one key in the house. It hangs on
 * a hook inside the birdcage on the basement's workbench (spec 13.55), and taking it puts it in
 * the backpack.
 */

typedef struct HouseKey {
    SceneNode* node; // where it hangs or is carried, and kept off the graph once it is taken
} HouseKey;

// The key, at the world's origin until it is placed.
void house_key_build(HouseKey* key, Engine* engine, Scene* scene);
void house_key_free(HouseKey* key);
// The key at `world`, in its own frame: centred, +z from its bow to its bit, x through its face.
void house_key_place(HouseKey* key, const mat4 world);

// The key alone, centred, along +d and standing on its edge, its bit up, so a picture from the
// side is its face.
void house_key_model(Kit* kit);

#endif // _SILENT_HOUSE_KEY_H_
