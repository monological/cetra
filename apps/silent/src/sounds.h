#ifndef _SILENT_SOUNDS_H_
#define _SILENT_SOUNDS_H_

#include <cglm/cglm.h>

#include "cetra/game/audio.h"

struct Door;

/*
 * The rooms sound passes between, as the engine's audio zones (spec 13.33) -- each house, the
 * home's living room, kitchen and basement, and the mansion's upper storey, joined by links,
 * three of them doors that open -- and the wind outside (spec 13.11). A sound placed in the
 * world is heard through the rooms by the engine. The wind, which rides over the listener, is
 * shaped here from what the outdoors reaches the listener with: two layers, full outdoors and
 * muffled indoors, crossfaded by it.
 */

// The doors whose swing opens a link: each house's front door, and the home's basement door.
typedef enum {
    SOUNDS_DOOR_NONE,
    SOUNDS_DOOR_HOME,
    SOUNDS_DOOR_MANSION,
    SOUNDS_DOOR_BASEMENT,
    SOUNDS_DOORS
} SoundsDoor;

typedef struct Sounds {
    AudioSystem* audio; // NULL without audio, as are the sounds
    Sound* wind_outside;
    Sound* wind_inside;
    const struct Door* doors[SOUNDS_DOORS]; // borrowed; NULL for one not hung
    AudioZoneLink door_links[SOUNDS_DOORS]; // the link each swings
    // What is left of the outdoors once a house's own walls are passed: 1 outdoors and in a
    // house's rooms, less below them. For a sound shaped like the outdoors that a roof has
    // already muffled its own way.
    float past_walls;
} Sounds;

// The rooms, and the wind started; `doors` indexed by SoundsDoor, entry 0 unused. `audio` may be
// NULL. The rooms go in before anything places a sound, so what plays while the rest loads is
// already heard through them.
void sounds_start(Sounds* sounds, AudioSystem* audio, const struct Door* const* doors);

// Per frame, before the game's audio update, so a door's link and the wind's place land this
// frame: each door's swing into its link, and the wind round the listener at `eye`.
void sounds_update(Sounds* sounds, const vec3 eye);

// A loop from a file, playing silent until its volume is set; NULL, with a line on stderr, when
// it cannot be loaded, and NULL without audio.
Sound* sounds_loop(AudioSystem* audio, const char* path);

#endif // _SILENT_SOUNDS_H_
