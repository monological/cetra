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
#include "cat_places.h"
#include "lights.h"

/*
 * The house's cat (spec 13.17): the body tools/make_cat_blender.py builds, brought in from
 * assets/models/cat.glb, given its colours and its coat (cat_body.c), set down at one of its
 * places, and sent from place to place over the house's graph (cat_places.h) by the controller
 * here.
 */

// Half the height of the body the player bumps into: the entity stands at its middle, so the
// feet are this far below it.
#define CAT_HALF_HEIGHT 0.16f

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
    // The body (cat_body.c).
    Entity* entity;     // NULL when there is no cat
    Animator* animator; // the entity's
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
    bool look_on;
    vec3 look_target; // world

    // What it does (cat.c).
    int clip;             // what the base layer plays
    float clip_seconds;   // how far into it
    bool held;            // a clip held from the command line: nothing moves the cat
    NavGraph* places;     // owned
    NavFollower follower; // the one record of where it is: at a place, or along a link
    CatMode mode;
    CatPosture posture;   // what it holds, or is shifting from
    CatPosture want;      // what it is making for once whatever it is doing is done
    CatPosture arrive_as; // what it takes up on arriving
    int goal;             // where it is going, -1 for nowhere
    CatGait gait;
    float yaw;                  // which way it faces, radians about +y, 0 facing +z
    float face;                 // the facing a turn on the spot is turning it to
    int leg;                    // the link whose clip is playing
    bool landed;                // the jump under way has put the cat down at its far end
    float jump_yaw0, jump_yaw1; // the facing a jump takes off with and lands with
    float takeoff, land;        // when in its clip the jump under way leaves and lands
    bool waiting;               // the player stands in its way
    int act;                    // the clip it plays in place, or stands up to play; -1 for none
    int go;                     // where to set off for once it is in the house, -1 to stay
    CatGait go_gait;
} Cat;

// The places a cat rests at, comma-separated, for the usage line.
const char* cat_place_list(void);

// Load the cat and stand it at its place, its body solid to the player but not yet drawn: see
// cat_update. False, with a line on stderr, when the model cannot be loaded.
bool cat_create(Cat* cat, const CatDesc* desc, Game* game, PhysicsWorld* physics);
// What the cat owns that its entity does not.
void cat_free(Cat* cat);

// Send it to a place, by the cheapest way there, to take `settle` on arriving (-1 for the
// posture the place says). From part way along a walk it goes on or turns back, whichever is
// shorter. False when there is no way there.
bool cat_go(Cat* cat, CatPlaceId place, CatGait gait, int settle);
// The same, by way of the gallery's hand rail where that is the way there: what a cat does
// when it means to walk the rail, and never on the way to anywhere else.
bool cat_go_by_rail(Cat* cat, CatPlaceId place, CatGait gait, int settle);
// The route cat_go would take there, by the rail or not; false when there is none.
bool cat_route(const Cat* cat, CatPlaceId place, bool by_rail, NavRoute* out);

// Take a posture where it is: at once out of a looping act, after a one-shot act or a transition
// under way. False while it is on its way somewhere.
bool cat_settle(Cat* cat, CatPosture posture);

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

// Its colours, sRGB 0..1, while it runs: a light coat takes pink ears and nose.
void cat_set_fur(Cat* cat, const vec3 srgb);
void cat_set_eyes(Cat* cat, const vec3 srgb);

// The place it stands at, or -1 between two.
int cat_place(const Cat* cat);
// At rest where it was going, nothing under way.
bool cat_settled(const Cat* cat);
// Where it was going and doing what it does there: resting, or an act in place.
bool cat_here(const Cat* cat);
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
// scene once the reflection probes have their pictures, its eyes' glow, and where its head
// turns. `viewer` is the player's eye.
void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt);

// One line of what it is doing, for --trace-cat.
void cat_trace(const Cat* cat, int step);

#endif // _SILENT_CAT_H_
