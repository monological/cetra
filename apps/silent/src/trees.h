#ifndef _SILENT_TREES_H_
#define _SILENT_TREES_H_

#include "cetra/procedural/tree_gen.h"
#include "cetra/scene.h"

#include "kit.h"

struct Engine;

#define TREE_MODELS 6

// The dead trees' shared parts (spec 13.25), grown once: the bark they sway in, the same bark on
// wood lying still, and the models, each at the generator's native size.
typedef struct Trees {
    Material* bark;
    Material* still_bark;
    Mesh* dead[TREE_MODELS];          // NULL where one would not grow
    float trunk_length, trunk_radius; // a model's, at its native size
    // Where each model's copies hang, so they are adjacent and draw together; from trees_build.
    SceneNode* groups[TREE_MODELS];
} Trees;

void trees_init(Trees* trees, struct Engine* engine, Scene* scene);

// The dead trees along the drive and round the mansion's grounds (spec 13.25): bare, twisted,
// swaying in the scene's wind, each standing on the hill with a collider round its trunk.
void trees_build(Trees* trees, Kit* kit, Scene* scene, unsigned int seed);

// A tree of `p` at the generator's native size: its wood in `bark`, and when `leaves` is not
// NULL its foliage in `foliage`. Either comes back NULL when it would not grow.
void trees_grow(const TreeParams* p, Material* bark, Material* foliage, Mesh** wood, Mesh** leaves);

// A dead model grown again in the still bark, to lie on the ground; NULL when it will not grow.
Mesh* trees_grow_still(Trees* trees, int model);

// Drops the models, once everything standing them about holds its own reference.
void trees_release(Trees* trees);

#endif // _SILENT_TREES_H_
