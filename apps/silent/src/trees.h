#ifndef _SILENT_TREES_H_
#define _SILENT_TREES_H_

#include "cetra/scene.h"

#include "kit.h"

struct Engine;

#define TREE_MODELS 6
#define TREE_TRUNK  8.0f // a dead model's native trunk radius

// The dead trees' shared parts (spec 13.25), grown once: the bark they sway in, the same bark on
// wood lying still, and the models, each at the generator's native size.
typedef struct Trees {
    Material* bark;
    Material* still_bark;
    Mesh* dead[TREE_MODELS]; // NULL where one would not grow
} Trees;

void trees_init(Trees* trees, struct Engine* engine, Scene* scene);

// The dead trees along the drive and round the mansion's grounds (spec 13.25): bare, twisted,
// swaying in the scene's wind, each standing on the hill with a collider round its trunk.
void trees_build(Trees* trees, Kit* kit, Scene* scene, unsigned int seed);

// A dead model grown again in the still bark, to lie on the ground; NULL when it will not grow.
Mesh* trees_grow_still(Trees* trees, int model);

// Drops the models, once everything standing them about holds its own reference.
void trees_release(Trees* trees);

#endif // _SILENT_TREES_H_
