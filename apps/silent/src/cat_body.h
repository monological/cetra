#ifndef _SILENT_CAT_BODY_H_
#define _SILENT_CAT_BODY_H_

#include "cat.h"

/*
 * The cat's body (spec 13.17): the model loaded through a scene of its own and moved under an
 * entity of silent's, with materials of silent's own carrying its colours and its coat; where
 * its eyes are and how they shine; where its rig is in the world. What it DOES is cat.c's.
 */

// Load the body and stand its entity with its feet at `feet`, facing `yaw`. False, with a line
// on stderr, when the model cannot be loaded.
bool cat_body_load(Cat* cat, const CatDesc* desc, Game* game, PhysicsWorld* physics,
                   const vec3 feet, float yaw);

// Where the rig's model space is in the world, from the entity's pose this step.
void cat_body_world(const Cat* cat, mat4 out);

// How bright the eyes would glow now, in nits, before easing.
float cat_body_shine(const Cat* cat, Game* game, const Lights* lights, const vec3 viewer);

#endif // _SILENT_CAT_BODY_H_
