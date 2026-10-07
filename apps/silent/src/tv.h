#ifndef _SILENT_TV_H_
#define _SILENT_TV_H_

#include <stdbool.h>

#include "cetra/game/audio.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The living room's television showing snow (spec 13.30). The picture is
 * shaders/tv_static.glsl on a card over the glass, in the late draw; this is
 * what it shares with the rest of the frame, each frame: the AGC's gain, which
 * the picture, the glass and the light all follow, and the picture's MEAN,
 * which the glass emits so that what reflects the screen before the late draw
 * sees it lit.
 */
typedef struct Tv {
    Material* glass;   // MAT_SCREEN: emits the picture's mean
    Material* picture; // MAT_TV_STATIC: the static's params
    Light* glow;       // the set's light on the room, NULL when the scene has none
    float glow_base;   // its intensity as authored, which the gain's mean scales
    float mean_base;   // the mean at a gain of 1, the authored intensity's
    Sound* hiss;       // the set's speaker on a dead channel, NULL without audio
} Tv;

// Finds the set's light and sets the picture's fixed params. With `on` false the
// glass keeps its own glow, as the set was before it showed anything.
void tv_init(Tv* tv, const Kit* kit, Scene* scene, bool on);
// The hiss, started at the set; nothing when the set is off or `audio` is NULL.
void tv_start_audio(Tv* tv, AudioSystem* audio);
// This frame's gain, onto the picture, the glass and the light, and the hiss at
// `hearing`, how much of the house's own sound reaches the listener.
void tv_update(Tv* tv, double time, float hearing);

#endif // _SILENT_TV_H_
