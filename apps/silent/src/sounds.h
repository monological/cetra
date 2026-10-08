#ifndef _SILENT_SOUNDS_H_
#define _SILENT_SOUNDS_H_

#include <cglm/cglm.h>

#include "cetra/game/audio.h"

/*
 * The house's sound beyond its clock and its tubes (spec 13.11): the fridge's hum and the wind
 * outside -- and since spec 13.33 the rooms sound passes between, as the engine's audio zones:
 * each house, the basement under the home, and the mansion's upper storey, joined by links,
 * three of them doors that open. A sound placed in the world is heard through them by the
 * engine, so nothing in silent multiplies a wall into a volume. The wind, which rides over the
 * listener, is shaped here from what the outdoors reaches the listener with: two layers, full
 * outdoors and muffled indoors, crossfaded by it.
 */
typedef struct Sounds {
    AudioSystem* audio; // NULL without audio, as are the rest
    Sound* fridge;
    Sound* wind_outside;
    Sound* wind_inside;
    // The links the doors swing: each house's front door's, and the basement door's.
    AudioZoneLink home_door;
    AudioZoneLink mansion_door;
    AudioZoneLink basement_door;
} Sounds;

// The rooms, and the loops started. `audio` may be NULL.
void sounds_start(Sounds* sounds, AudioSystem* audio);

// Per frame, before the listener moves: each door's swing, 0 shut to 1 open, into its link, and
// the wind round the listener at `eye`.
void sounds_update(Sounds* sounds, const vec3 eye, float home_door, float mansion_door,
                   float basement_door);

// What is left of the outdoors once a house's own walls are passed: 1 outdoors and in a house's
// rooms, less in the basement under them. For a sound shaped like the outdoors that a roof has
// already muffled its own way -- the rain.
float sounds_past_walls(const Sounds* sounds);

// A loop from a file, playing silent until its volume is set; NULL, with a line on stderr, when
// it cannot be loaded, and NULL without audio.
Sound* sounds_loop(AudioSystem* audio, const char* path);

#endif // _SILENT_SOUNDS_H_
