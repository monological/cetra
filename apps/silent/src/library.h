#ifndef _SILENT_LIBRARY_H_
#define _SILENT_LIBRARY_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

/*
 * A model from a file brought into the game (spec 13.55), the way the cat's is (spec 13.17): the
 * file is loaded as a scene of its own, a LIBRARY the engine owns and never draws, which keeps the
 * skeletons and clips the model's animator borrows; the model's nodes are moved out of it under
 * one node of the game's, on no graph until the caller puts it on one; and every material is moved
 * to the game's scene, which frees it, its textures with it.
 */

// The model in `path` under a node named `name`, its file's own frame, and its library in
// `*library` when that is not NULL; NULL, said by name, when it cannot be loaded. Its textures are
// in when this returns: two files' embedded pictures go by the same names, so no load is left in
// flight for the next file's to answer.
SceneNode* library_take(Engine* engine, Scene* game, const char* path, const char* name,
                        Scene** library);

// The node named `name` under `root` and its frame in `root`'s, every transform between them taken
// in; NULL when there is none.
SceneNode* library_find(SceneNode* root, const char* name, mat4 frame);

#endif // _SILENT_LIBRARY_H_
