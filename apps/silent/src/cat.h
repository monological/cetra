#ifndef _SILENT_CAT_H_
#define _SILENT_CAT_H_

#include <stdbool.h>

#include <cglm/cglm.h>

#include "cetra/animation.h"
#include "cetra/animator.h"
#include "cetra/material.h"
#include "cetra/nav_graph.h"
#include "cetra/scene.h"

#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/physics.h"

#include "cat_clips.h"
#include "lights.h"

/*
 * The house's cat (spec 13.17): the body tools/make_cat_blender.py builds, brought in from
 * assets/models/cat.glb, given its colours and its coat, set down at one of its places, and
 * sent from place to place over the house's graph (cat_places.h).
 */

typedef struct CatDesc {
    vec3 fur;           // sRGB 0..1
    vec3 eyes;          // sRGB 0..1
    const char* at;     // a place by name; NULL is home, the study chair
    const char* clip;   // a clip held there instead of resting; NULL rests as the place says
    float clip_seconds; // that clip held this far in; below 0 it plays
    const char* go;     // a place to go to as soon as it is in the house; NULL stays
    bool trot;          // and go at a trot
    bool eyeshine;      // the eyes throw the flashlight back
} CatDesc;

// How it holds itself, lowest first: each is one transition clip from the next.
typedef enum { CAT_CURL, CAT_LIE, CAT_SIT, CAT_STAND } CatPosture;

typedef enum {
    CAT_RESTING,  // holding its posture
    CAT_SHIFTING, // a transition between two postures
    CAT_TURNING,  // turning on the spot
    CAT_WALKING,  // along walks and flights
    CAT_JUMPING,  // through a jump's arc
} CatMode;

typedef struct Cat {
    Entity* entity;     // NULL when there is no cat
    Animator* animator; // the entity's
    SceneNode* holder;  // the entity's node, in the scene once attached
    SceneNode* skin;    // the node carrying the skinned meshes
    Material* eye;      // what glows when the flashlight catches it
    int eye_bones[2];   // left and right; -1 when the rig lacks one
    bool eyeshine;
    float shine; // the eyes' glow now, nits
    bool attached;

    const Animation* clips[CAT_CLIP_COUNT];
    int clip;           // what the base layer plays
    float clip_seconds; // how far into it
    bool held;          // a clip held from the command line: nothing moves the cat

    NavGraph* places; // owned
    NavFollower follower;
    CatMode mode;
    CatPosture posture; // what it holds, or is shifting from
    CatPosture want;    // what it is shifting to
    int at;             // the place it is at, or last left
    int goal;           // where it is going, -1 for nowhere
    bool trot;
    float yaw;      // which way it faces, radians about +y, 0 facing +z
    float face;     // the facing a turn on the spot is turning it to
    bool turn_clip; // turning by a quarter-turn clip, rather than easing round
    int leg;        // the link whose clip is playing
    bool landed;    // the jump under way has put the cat down at its far end
    float blocked;  // seconds the player has stood in its way
    const char* go; // where to set off for once it is in the house, or NULL
    bool go_trot;
} Cat;

// The places a cat rests at, comma-separated, for the usage line.
const char* cat_place_list(void);

// Load the cat and stand it at its place, its body solid to the player but not yet drawn: see
// cat_update. False, with a line on stderr, when the model cannot be loaded.
bool cat_create(Cat* cat, const CatDesc* desc, Game* game, Scene* scene, PhysicsWorld* physics);

// Send it to a place by name, at a walk or a trot, by the cheapest way there. False when there
// is no such place, or no way to it.
bool cat_go(Cat* cat, const char* place, bool trot);

// Each fixed step: what it is doing, and where that puts it. `player` is the player's feet,
// which it waits for rather than walks through.
void cat_step(Cat* cat, const vec3 player, float dt);

// Once a frame before the frame draws, after the flashlight has moved: puts the cat in the
// scene once the reflection probes have their pictures, and its eyes' glow. `viewer` is the
// player's eye.
void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt);

// One line of what it is doing, for --trace-cat.
void cat_trace(const Cat* cat, int step);

#endif // _SILENT_CAT_H_
