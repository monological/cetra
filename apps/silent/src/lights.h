#ifndef _SILENT_LIGHTS_H_
#define _SILENT_LIGHTS_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"

#include "kit.h"

#define LIGHTS_MAX_TUBES 4

/*
 * The house's light: the kitchen's fluorescent tubes, a bare bulb in the hall,
 * and the player's flashlight.
 *
 * A tube is a flat EMISSIVE strip in a mesh of its own, and the engine's
 * emissive-light pass turns each one into an LTC area panel every frame from
 * the material's emissive. So the light IS the glowing surface: a flicker
 * changes the material's strength and the panel follows it, and irradiance
 * captures leave the strip out, since the panel already carries its light.
 */
typedef struct Lights {
    Material* flicker;  // the failing tube's strip, or NULL when none flickers
    float flicker_nits; // and its brightness when it holds
    int flicker_tube;   // which tube that is; -1 when none
    unsigned int seed;

    Sound* buzz[LIGHTS_MAX_TUBES]; // each tube's ballast, NULL without audio
    bool buzzing[LIGHTS_MAX_TUBES];

    Light* flashlight;
    bool flashlight_on;
    vec3 flashlight_dir; // smoothed, so the beam trails the look a little
} Lights;

// The fixtures' bodies go into the kit; the strips and the flashlight straight
// into the scene. Call before kit_finish.
void lights_build(Lights* lights, Kit* kit, Engine* engine, Scene* scene, unsigned int seed,
                  bool flicker, bool flashlight_on);

// A buzz at each tube, from `audio`, which may be NULL.
void lights_start_audio(Lights* lights, AudioSystem* audio);

// Per frame, before the frame draws: the flicker, the tubes' shadows and
// buzz, and the flashlight riding the eye.
void lights_update(Lights* lights, Scene* scene, double time, float dt, const vec3 eye,
                   const vec3 forward);

void lights_toggle_flashlight(Lights* lights);

// A bare bulb on a failing supply (spec 13.31): 1 most of the time, and now and then dimming
// for a second or so to between 0.3 and 0.65 and coming back. A pure function of the sim clock.
float lights_brownout(double t, unsigned int seed);

#endif // _SILENT_LIGHTS_H_
