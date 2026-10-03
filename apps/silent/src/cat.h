#ifndef _SILENT_CAT_H_
#define _SILENT_CAT_H_

#include <stdbool.h>

#include <cglm/cglm.h>

#include "cetra/animation.h"
#include "cetra/animator.h"
#include "cetra/look_at.h"
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

// How fast it goes along a level walk; a flight and a jump go at their own pace.
typedef enum { CAT_WALK, CAT_TROT, CAT_RUN } CatGait;

typedef struct CatDesc {
    vec3 fur;           // sRGB 0..1
    vec3 eyes;          // sRGB 0..1
    const char* at;     // a place by name; NULL is home, the study chair
    const char* clip;   // a clip held there instead of resting; NULL rests as the place says
    float clip_seconds; // that clip held this far in; below 0 it plays
    const char* go;     // a place to go to as soon as it is in the house; NULL stays
    CatGait gait;       // and how fast
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
    CAT_ACTING,   // a clip in place: a stretch, a startle, grooming, hissing
} CatMode;

typedef struct Cat {
    Entity* entity;     // NULL when there is no cat
    Animator* animator; // the entity's
    SceneNode* holder;  // the entity's node, in the scene once attached
    SceneNode* skin;    // the node carrying the skinned meshes
    Material* eye;      // what glows when the flashlight catches it
    Material* fur;
    Material* ear; // inside the ears
    Material* nose;
    vec3 ear_dark, nose_dark; // the two as modelled, which a light coat turns pink
    vec3 fur_srgb, eyes_srgb; // the colours asked for
    int eye_bones[2];         // left and right; -1 when the rig lacks one
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
    int shift_dir;      // the transition under way goes up a posture (1) or down one (-1)
    int at;             // the place it is at, or last left
    int goal;           // where it is going, -1 for nowhere
    CatGait gait;
    int settle;                 // the posture to take on arriving, -1 for the one the place says
    float yaw;                  // which way it faces, radians about +y, 0 facing +z
    float face;                 // the facing a turn on the spot is turning it to
    bool turn_clip;             // turning by a quarter-turn clip, rather than easing round
    int leg;                    // the link whose clip is playing
    bool landed;                // the jump under way has put the cat down at its far end
    float jump_yaw0, jump_yaw1; // the facing a jump takes off with and lands with
    float blocked;              // seconds the player has stood in its way
    const char* go;             // where to set off for once it is in the house, or NULL
    CatGait go_gait;

    int act;             // the clip played in place, -1 for none
    int act_pending;     // an act waiting for the cat to stand up, -1 for none
    CatPosture act_ends; // the posture the act leaves it in
    int vocal;           // what the override layer plays, -1 for nothing
    float vocal_seconds; // how far into it
    LookAtSystem* look;  // the animation state's; NULL without a neck
    bool look_on;
    vec3 look_target; // world
} Cat;

// The places a cat rests at, comma-separated, for the usage line.
const char* cat_place_list(void);

// Load the cat and stand it at its place, its body solid to the player but not yet drawn: see
// cat_update. False, with a line on stderr, when the model cannot be loaded.
bool cat_create(Cat* cat, const CatDesc* desc, Game* game, Scene* scene, PhysicsWorld* physics);

// Send it to a place by name, by the cheapest way there, to take `settle` on arriving (-1 for
// the posture the place says). From part way along a walk it goes on or turns back, whichever
// is shorter. False when there is no such place, or no way to it.
bool cat_go(Cat* cat, const char* place, CatGait gait, int settle);
// The same, by way of the gallery's hand rail where that is the way there: what a cat does
// when it means to walk the rail, and never on the way to anywhere else.
bool cat_go_by_rail(Cat* cat, const char* place, int settle);

// Play a clip in place: CAT_CLIP_STRETCH (standing up first), CAT_CLIP_STARTLE (from anything
// but a jump, a flight or a turn, stopping where it is), CAT_CLIP_HISS (standing), or
// CAT_CLIP_GROOM (sitting). A looping one plays until the cat is sent somewhere or given
// another. False when it cannot be played now.
bool cat_act(Cat* cat, int clip);

// A vocal or a flick of the face over whatever the body plays: meows, the trill, the yawn,
// blinks and ear flicks. False while one is already playing.
bool cat_vocal(Cat* cat, int clip);

// Turn the head toward a world point while it can, or back to the clip with NULL.
void cat_look(Cat* cat, const vec3 world);

// Its coat and its eyes, sRGB 0..1, while it runs: a light coat takes pink ears and nose.
void cat_set_fur(Cat* cat, const vec3 srgb);
void cat_set_eyes(Cat* cat, const vec3 srgb);

// At rest where it was going, nothing under way.
bool cat_settled(const Cat* cat);
// Asleep, or with its eyes shut.
bool cat_eyes_shut(const Cat* cat);
// Where its eyes are, between them, and the way its head faces, from the pose last drawn.
bool cat_eye(const Cat* cat, vec3 at, vec3 forward);
// Where its feet are.
void cat_feet(const Cat* cat, vec3 out);

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
