#ifndef _SILENT_LIGHTS_H_
#define _SILENT_LIGHTS_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/scene.h"

#include "kit.h"

#define LIGHTS_MAX_TUBES 4

/*
 * The kitchen's light: fluorescent tubes and the player's flashlight.
 *
 * A tube is a flat EMISSIVE strip in a mesh of its own, and the engine's
 * emissive-light pass turns each one into an LTC area panel every frame from
 * the material's emissive. So the light IS the glowing surface: a flicker
 * changes the material's strength and the panel follows it, and irradiance
 * captures leave the strip out, since the panel already carries its light.
 */
typedef struct Lights {
    Material* tube_materials[LIGHTS_MAX_TUBES];
    float tube_nits[LIGHTS_MAX_TUBES];
    int tube_count;
    int flicker_tube; // index into the above, or -1
    unsigned int seed;

    Light* flashlight;
    float flashlight_candela;
    bool flashlight_on;
    vec3 flashlight_dir; // smoothed, so the beam trails the look a little
} Lights;

// The fixtures' bodies go into the kit; the strips and the flashlight straight
// into the scene. Call before kit_finish.
void lights_build(Lights* lights, Kit* kit, Engine* engine, Scene* scene, unsigned int seed,
                  bool flicker, bool flashlight_on);

// Per frame, before the frame draws: the flicker, the tubes' shadows, and the
// flashlight riding the eye.
void lights_update(Lights* lights, Scene* scene, double time, float dt, const vec3 eye,
                   const vec3 forward);

void lights_toggle_flashlight(Lights* lights);

#endif // _SILENT_LIGHTS_H_
