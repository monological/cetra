#ifndef _SILENT_SOUNDS_H_
#define _SILENT_SOUNDS_H_

#include <cglm/cglm.h>

#include "cetra/game/audio.h"

/*
 * The house's sound beyond its clock and its tubes (spec 13.11): the fridge's
 * hum, the wind outside, and how much of each reaches the listener.
 *
 * The engine's sound has no walls -- everything falls off with straight-line
 * distance -- so the house fakes them with one number, INSIDE: 1 well inside
 * its footprint, 0 well outside, blended across the walls. The wind is two
 * layers, heard full outdoors and muffled indoors, crossfaded by it, and every
 * sound the house makes itself is dimmed by it out on the street.
 */
typedef struct Sounds {
    Sound* fridge; // NULL without audio, as are the rest
    Sound* wind_outside;
    Sound* wind_inside;
    float inside; // 0..1, following the listener with a short lag
    float below;  // 0..1, how far the listener is under the ground, in the basement (spec 13.31)
} Sounds;

// The loops, started, for a listener at `eye`. `audio` may be NULL.
void sounds_start(Sounds* sounds, AudioSystem* audio, const vec3 eye);

// Per frame, before the listener moves: where the listener is decides INSIDE,
// and INSIDE the mix.
void sounds_update(Sounds* sounds, const vec3 eye, float dt);

// How much of a sound the house makes reaches the listener: 1 inside, less
// from the street.
float sounds_indoor_gain(const Sounds* sounds);

// The same for a sound made at `source`, which is quieter again a storey away from the
// listener, unless the listener stands in the great hall, which is open to the gallery.
float sounds_gain_at(const Sounds* sounds, const vec3 listener, const vec3 source);

// What reaches the listener of a sound made above the ground -- the house's rooms, the rain on
// its roof -- through the floor overhead: 1 above it, less down in the basement.
float sounds_overhead_gain(const Sounds* sounds);

// A loop from a file, playing silent until its volume is set; NULL, with a line on stderr, when
// it cannot be loaded, and NULL without audio.
Sound* sounds_loop(AudioSystem* audio, const char* path);

#endif // _SILENT_SOUNDS_H_
