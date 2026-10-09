#ifndef _SILENT_TV_H_
#define _SILENT_TV_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/game/audio.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The living room's television (spec 13.30): the set, and, switched on, snow on it with its light
 * on the room and its hiss. The picture is shaders/tv_static.glsl on a card over the glass, drawn
 * in the late draw; the glass emits the picture's expected light (shaders/tv_glass.glsl), so what
 * sees the glass before the late draw -- SSR, the probes, TAA's history -- sees the screen lit.
 */
typedef struct Tv {
    Material* glass;   // emits the picture's mean
    Material* picture; // the snow; NULL when the set shows none
    Light* glow;       // the set's light on the room, a panel over the picture
    float average;     // the picture's light over its area, as a share of the signal's mean
    vec3 speaker;      // world: where its hiss comes from
    vec4 field;        // the field last shown: its number, the hum's phase, spread and mean
} Tv;

// Lays the set on its stand in the living room and hangs its light; with `on`, the picture over
// the glass and its hiss. Into `kit` before kit_finish, which takes what it lays.
void tv_build(Tv* tv, Kit* kit, Engine* engine, Scene* scene, bool on);
// The hiss, started at the set; nothing when it shows no picture or `audio` is NULL.
void tv_start_audio(Tv* tv, AudioSystem* audio);
// The field at `time` onto the picture, the glass and the light.
void tv_update(Tv* tv, double time);
// For the light's captures (spec 13.42): `rest` shows a field at the signal's long-run mean, with
// no breath in its gain; false the field the last update showed.
void tv_rest(Tv* tv, bool rest);

#endif // _SILENT_TV_H_
