#ifndef _SILENT_ARMS_H_
#define _SILENT_ARMS_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/animator.h"
#include "cetra/engine.h"
#include "cetra/scene.h"

#include "arms_clips.h"

/*
 * The player's arms (spec 13.55): bare hands out of an olive field jacket's sleeves, shown only
 * while something is held. tools/make_arms_blender.py builds and animates them into
 * assets/models/arms.glb, and writes the clips' timing into arms_clips.h.
 *
 * They stand at the BODY: the standing eye, facing the way the body faces, x its left and y up.
 * Every clip is authored from there, and three bones that skin nothing say where a clip puts what
 * is not the arms: the camera, the held cage and the flashlight's beam.
 *
 * Their animator is the app's, ticked once a rendered frame on the clock the view eases by: an
 * entity's is ticked in the sim clock's whole steps, which this close to the eye would judder.
 */

typedef enum {
    ARMS_AT_VIEW, // the eye: +Y where it looks, +Z up
    ARMS_AT_CAGE, // the held cage's own frame, at its base's middle
    ARMS_AT_LAMP, // the beam, from the lens on the chest: +Y where it shines
    ARMS_AT_KEY,  // the key in the pinch, in its own frame, while the take carries it
    ARMS_AT_COUNT
} ArmsAt;

typedef struct Arms {
    SceneNode* node; // the arms, at the body; on the graph only while shown
    SceneNode* root; // the scene's, which they go under
    Animator* animator;
    const Animation* clips[ARMS_CLIP_COUNT];
    ArmsClipId playing;
    int bones[ARMS_AT_COUNT];
    mat4 bind[ARMS_AT_COUNT]; // each of those bones' frames at bind, in the skin's frame
    mat4 skin;                // the skinned meshes' frame in the node's
    mat4 body;                // where the body was last put, in the world
    bool shown;
} Arms;

// The arms, off the graph, playing nothing; false, said by name, when the model or its rig cannot
// be had, and the game goes on without them.
bool arms_load(Arms* arms, Engine* engine, Scene* scene);
void arms_free(Arms* arms);

// On the graph or off it.
void arms_show(Arms* arms, bool shown);
// `clip` from its start, crossfaded from what played over `fade` seconds; looping as it is made.
void arms_play(Arms* arms, ArmsClipId clip, float fade);
// Per rendered frame, before anything reads them: the body at `body` in the world, and the clip
// `dt` seconds on. `events` hears each of the clip's events as it is passed, with `user`.
void arms_frame(Arms* arms, const mat4 body, float dt, AnimatorEventFn events, void* user);
// Seconds into the clip playing, and whether a clip that does not loop has come to its end.
float arms_seconds(const Arms* arms);
bool arms_done(const Arms* arms);
// Where `at` is in the world, from the pose of the last frame.
void arms_at(const Arms* arms, ArmsAt at, mat4 out);

#endif // _SILENT_ARMS_H_
