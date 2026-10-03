#ifndef _SILENT_CAT_H_
#define _SILENT_CAT_H_

#include <stdbool.h>

#include <cglm/cglm.h>

#include "cetra/animator.h"
#include "cetra/material.h"
#include "cetra/scene.h"

#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/physics.h"

#include "lights.h"

/*
 * The house's cat (spec 13.17): the body tools/make_cat_blender.py builds, brought in from
 * assets/models/cat.glb, given its colours and its coat, and set down at one of its places.
 */

typedef struct CatDesc {
    vec3 fur;           // sRGB 0..1
    vec3 eyes;          // sRGB 0..1
    const char* at;     // a place by name; NULL is home, the study chair
    const char* clip;   // a clip by name instead of the place's own; NULL is the place's
    float clip_seconds; // held this far into the clip; below 0 the clip plays
    bool eyeshine;      // the eyes throw the flashlight back
} CatDesc;

typedef struct Cat {
    Entity* entity;     // NULL when there is no cat
    Animator* animator; // the entity's
    SceneNode* holder;  // the entity's node, in the scene once attached
    SceneNode* skin;    // the node carrying the skinned meshes
    Material* eye;      // what glows when the flashlight catches it
    int eye_bones[2];   // left and right; -1 when the rig lacks one
    bool eyes_shut;     // the clip it holds keeps its lids closed
    bool eyeshine;
    float shine; // the eyes' glow now, nits
    bool attached;
} Cat;

// The places a cat can be set down at, comma-separated, for the usage line.
extern const char* const CAT_PLACE_LIST;

// Load the cat and stand it at its place, its body solid to the player but not yet drawn: see
// cat_update. False, with a line on stderr, when the model cannot be loaded.
bool cat_create(Cat* cat, const CatDesc* desc, Game* game, Scene* scene, PhysicsWorld* physics);

// Once a frame before the frame draws, after the flashlight has moved: puts the cat in the
// scene once the reflection probes have their pictures, and its eyes' glow. `viewer` is the
// player's eye.
void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt);

#endif // _SILENT_CAT_H_
