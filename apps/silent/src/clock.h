#ifndef _SILENT_CLOCK_H_
#define _SILENT_CLOCK_H_

#include "cetra/engine.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"

#include "kit.h"

/*
 * The hall clock (spec 13.10): a Georgian longcase against the hall's west
 * wall, facing the kitchen door, so it is the first thing through the doorway.
 * The case is static and goes into the world's kit with everything else. The
 * pendulum and the three hands are each a node of their own, built round the
 * point they turn about and set every frame from the sim clock, so a headless
 * run shows the same swing on the same frame every time.
 */
typedef struct Clock {
    SceneNode* pendulum;
    SceneNode* hour;
    SceneNode* minute;
    SceneNode* second;
    Sound* tick; // NULL without audio
    Sound* tock;
    long beat; // the last beat heard; below zero before the first frame
} Clock;

// The case, into the world's kit. Call before kit_finish.
void clock_build(Kit* kit);

// The moving parts, and the beat's two sounds from `audio`, which may be NULL.
void clock_start(Clock* clock, Engine* engine, Scene* scene, AudioSystem* audio);

// Per frame, before the transform walk: the swing, the hands and the beat.
// `hearing` scales the beat for where the listener is.
void clock_update(Clock* clock, double time, float hearing);

// Where the pendulum's bob is in the world at sim time `time`: what a cat watches.
void clock_bob(double time, vec3 out);

#endif // _SILENT_CLOCK_H_
