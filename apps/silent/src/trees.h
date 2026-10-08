#ifndef _SILENT_TREES_H_
#define _SILENT_TREES_H_

#include "cetra/procedural/tree_gen.h"
#include "cetra/scene.h"

#include "kit.h"

struct Engine;

#define TREE_MODELS 6

// How far the wind moves a tree, its wood and its leaves alike (Material.wind_response): stiff,
// as dead wood and a wind-bent conifer are.
#define TREES_WIND_RESPONSE 0.35f

// How far through the fog a tree is drawn: the fog's extinction (street.c), 0.09 a metre by night
// and 0.14 by day, leaves a tree about 2% of its contrast at these.
#define TREES_REACH_NIGHT 45.0f
#define TREES_REACH_DAY   30.0f

// The dead trees' shared parts (spec 13.35), grown once: the bark they sway in, the same bark on
// wood lying still, and the models, each at the generator's native size.
typedef struct Trees {
    Material* bark;
    Material* still_bark;
    Mesh* dead[TREE_MODELS];         // NULL where one would not grow
    float trunk_length;              // the models', at their native size
    float trunk_radius[TREE_MODELS]; // each model's, at its native size
    // Where each model's copies hang, so they are adjacent and draw together; from trees_build.
    SceneNode* groups[TREE_MODELS];
    // The draw distance every tree is hung under, TREES_REACH_* in fog; 0 = drawn however far.
    // Set before trees_build.
    float reach;
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

// The share of a trunk's base radius it keeps at a body's height, a little up its taper, and how
// tall a trunk's body stands: past anyone's head.
#define TREES_TRUNK_BODY   0.85f
#define TREES_TRUNK_HEIGHT 3.0f

// A standing trunk's body: a box `radius` each way, from the ground at `ground` up `height`,
// centred where a trunk leaning `lean` radians off upright, toward `yaw`, is at half that height
// -- so a leaning tree's body leans with it rather than standing beside it. `yaw` and `lean` are
// the tree's own: its node turned by yaw about Y, then tipped by lean about X.
void trees_trunk_collider(Kit* kit, float x, float z, float ground, float radius, float height,
                          float yaw, float lean);

// Drops the models, once everything standing them about holds its own reference.
void trees_release(Trees* trees);

#endif // _SILENT_TREES_H_
