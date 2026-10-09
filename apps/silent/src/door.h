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
 * A door that opens and shuts when the player asks (spec 13.13): a leaf shaped to its opening
 * -- the Gothic house's iron-strapped planks, or the player's house's four panels (spec 13.25)
 * -- hung on a hinge and swung between shut and open. Nothing pushes it: it is a KINEMATIC
 * body, solid at every angle, and it moves only when it is told to and only by its own easing,
 * so a headless run swings it the same way every time.
 */

#define DOOR_CLEARANCE 0.008f // a leaf's gap to its opening, all round

// A leaf of `o`'s outline, `t` thick about d = 0 in frame `f`, its hinge at o->from.
typedef void (*DoorLeafFn)(Kit* kit, const KitFrame* f, const KitOpening* o, float t);
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

// Builds `leaf` as a node and a body of its own, shut, hung at `hinge`'s origin in its frame.
// `shape`'s from/to are along the frame from the hinge, its bottom and top in world y. False if
// it has no body, and is then no door.
bool door_build(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics,
                const char* name, DoorLeafFn leaf, const KitFrame* hinge, const KitOpening* shape,
                float thick, float swing);
// The same for a door hung in `opening` as its wall has it, from a hinge at one of its jambs: a
// leaf DOOR_THICK thick filling the opening shy of a clearance all round, its foot just off the
// floor.
bool door_hang(Door* door, Engine* engine, Scene* scene, EntityManager* em, PhysicsWorld* physics,
               const char* name, DoorLeafFn leaf, const KitFrame* hinge, KitOpening opening,
               float swing);

// The leaves, which a door that never moves builds into the kit it is given. The Gothic one's
// straps are on its -d face, the battened one's ledges on its +d face; the panelled one is the
// same both sides.
void door_leaf(Kit* kit, const KitFrame* f, const KitOpening* o, float t);
void door_leaf_panelled(Kit* kit, const KitFrame* f, const KitOpening* o, float t);
void door_leaf_battened(Kit* kit, const KitFrame* f, const KitOpening* o, float t);

// Toward open if it is shut or shutting, toward shut otherwise.
void door_toggle(Door* door);
// Whether the next toggle opens it.
bool door_will_open(const Door* door);

// Per fixed step: eases the leaf and moves its body and node.
void door_update(Door* door, float dt);

#endif // _SILENT_DOOR_H_
