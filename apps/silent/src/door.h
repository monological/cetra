#ifndef _SILENT_DOOR_H_
#define _SILENT_DOOR_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"

#include "kit.h"

/*
 * A door that opens and shuts when the player asks (spec 13.13): a Gothic plank leaf shaped
 * to its opening, iron-strapped, with a ring to pull, hung on a hinge and swung between shut
 * and open. Nothing pushes it: it is a KINEMATIC body, solid at every angle, and it moves only
 * when it is told to and only by its own easing, so a headless run swings it the same way
 * every time.
 */
typedef struct Door {
    // Where it hangs: the hinge's foot in the world, and the yaw of the frame the shut leaf
    // lies in -- a along it from the hinge, d into the room it opens into. Its straps are on
    // its -d face, the outside.
    vec3 hinge;
    float yaw;
    KitOpening shape; // the leaf's outline: a from the hinge (0..width), y in the world
    float swing;      // radians it turns, toward +d, to stand open
    // How far along its swing it is, 0 shut .. 1 open, travelling toward `want`.
    float travel;
    float want;
    Entity* entity;
} Door;

// Builds the leaf as a node and a body of its own, shut, hung at `hinge`'s origin in its
// frame. `shape`'s from/to are along the frame from the hinge, its bottom and top in world y.
// False if it has no body, and is then no door.
bool door_build(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics,
                const char* name, const KitFrame* hinge, const KitOpening* shape, float thick,
                float swing);

// A leaf of `o`'s outline, `t` thick about d = 0 in frame `f`, as a door that never moves is
// built: the same leaf, in the kit it is given.
void door_leaf(Kit* kit, const KitFrame* f, const KitOpening* o, float t);

// Toward open if it is shut or shutting, toward shut otherwise.
void door_toggle(Door* door);
// Whether the next toggle opens it.
bool door_will_open(const Door* door);

// Per fixed step: eases the leaf and moves its body and node.
void door_update(Door* door, float dt);

// How far the eye is from the middle of the leaf's face when the door is in reach -- within
// `reach` metres and less than `cone` radians off the view -- and FLT_MAX when it is not.
float door_reach_distance(const Door* door, const vec3 eye, const vec3 forward, float reach,
                          float cone);

#endif // _SILENT_DOOR_H_
