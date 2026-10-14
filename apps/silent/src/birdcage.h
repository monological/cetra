#ifndef _SILENT_BIRDCAGE_H_
#define _SILENT_BIRDCAGE_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

/*
 * The birdcage the front door's key is kept in (spec 13.55): a small cage of rusted iron, cast
 * solid, with a face for its door. tools/make_birdcage_blender.py builds and bakes it into
 * assets/models/birdcage.glb, in its own frame: x its left, y up, z out of its face, its foot at 0.
 * It stands on the basement's workbench where the key used to lie, its face to whoever stands at
 * the bench, and goes from there into the player's hands and back.
 */

typedef struct Birdcage {
    SceneNode* node; // the cage, on the bench or in the hands
    SceneNode* door; // its face, turned about its hinge
    mat4 door_rest;  // the door's own transform, shut
    mat4 hang;       // where the key hangs on its hook, in the cage's frame
    mat4 bench;      // where the cage stands on the bench, in the world
    vec3 at;         // its middle there, which the player reaches for
} Birdcage;

// The cage on the bench, shut; false, said by name, when its model cannot be had.
bool birdcage_build(Birdcage* cage, Engine* engine, Scene* scene);
void birdcage_free(Birdcage* cage);

// The cage at `world`, its frame's.
void birdcage_place(Birdcage* cage, const mat4 world);
// Its door turned `angle` radians about its hinge: 0 is shut, and a negative angle opens it.
void birdcage_door(Birdcage* cage, float angle);
// Where the key hangs inside, in the key's own frame, wherever the cage now is.
void birdcage_key(const Birdcage* cage, mat4 out);

#endif // _SILENT_BIRDCAGE_H_
