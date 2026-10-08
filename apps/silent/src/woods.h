#ifndef _SILENT_WOODS_H_
#define _SILENT_WOODS_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "fences.h"
#include "kit.h"
#include "trees.h"

// One tree of the woods: where it stands, and the nodes it draws as, each under its model's group
// so a model's copies stay adjacent and draw together.
typedef struct WoodsTree {
    float x, z;
    SceneNode* nodes[2]; // the wood, and the foliage when it has any
    SceneNode* groups[2];
    bool shown;
} WoodsTree;

typedef struct Woods {
    WoodsTree* trees;
    int count;
    float reach; // metres from the eye within which a tree is drawn
} Woods;

/*
 * The woods behind both sides of the street (spec 13.35): conifers from the engine's generator,
 * with the dead trees mixed in, stumps, fallen logs and boulders on a floor of earth and twigs,
 * thinner along the fences and closing up behind. A dead tree lies across each fence that has come
 * down. `night` draws further, the fog being thinner.
 */
void woods_build(Woods* woods, Kit* kit, Engine* engine, Scene* scene, Trees* trees,
                 const FenceBreaches* breaches, unsigned int seed, bool night);

// Draws the trees within reach of the eye and drops the rest: each frame, before the draw.
void woods_update(Woods* woods, const vec3 eye);

// Frees the trees out of reach, which hang on no node the scene would free.
void woods_free(Woods* woods);

#endif // _SILENT_WOODS_H_
