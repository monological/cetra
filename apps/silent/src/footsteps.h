#ifndef _SILENT_FOOTSTEPS_H_
#define _SILENT_FOOTSTEPS_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "cetra/gait.h"
#include "cetra/game/audio.h"
#include "cetra/game/entity.h"

/*
 * The player's footsteps (spec 13.47): a recording of what the feet are on, played where the
 * foot comes down, each time the gait lets one fall.
 *
 * What the feet are on is read off the plan, as the cat's paws are, since no collider carries a
 * material: the lake and its muddy edge, a rug, the houses' floors, the cabin and the dock, the
 * street, the drive and the track, the woods, and grass everywhere else. Out in the rain, under
 * open sky, hard ground and grass are wet and sound it.
 *
 * Each surface has a few takes, from fetch_sounds.py; a footfall plays one it did not play last,
 * a little louder or quieter and higher or lower each time, from a seeded generator so a headless
 * run repeats. Every recording is levelled alike, so the volumes in footsteps.c are the whole
 * statement of how loud each is.
 */

typedef enum FootSurface {
    FOOT_WOOD,
    FOOT_TILE,
    FOOT_CLOTH,
    FOOT_CONCRETE,
    FOOT_PAVEMENT_WET,
    FOOT_GRAVEL,
    FOOT_GRASS,
    FOOT_GRASS_WET,
    FOOT_FOREST,
    FOOT_MUD,
    FOOT_WATER,
    FOOT_SURFACES
} FootSurface;

#define FOOTSTEP_TAKES 6

typedef struct Footsteps {
    AudioSystem* audio; // NULL: silent
    const Entity* body; // what the voices ride
    Sound* takes[FOOT_SURFACES][FOOTSTEP_TAKES];
    int count[FOOT_SURFACES];
    int last[FOOT_SURFACES]; // the take played last, which is not played next
    uint32_t rng;
    int said; // takes played by footsteps_say
    bool trace;
} Footsteps;

// The takes there are of each surface, from `audio`, which may be NULL; the voices ride `body`.
void footsteps_start(Footsteps* f, AudioSystem* audio, const Entity* body, uint32_t seed);

// What feet at `feet` stand on; `wet` is ground out under the rain.
FootSurface footsteps_surface(const vec3 feet, bool wet);
const char* footsteps_surface_name(FootSurface surface);

// A foot came down on `surface`: `feet` is where the feet are in the body's frame and `right` the
// body's right, along which the foot that fell stands off.
void footsteps_fall(Footsteps* f, const GaitFootfall* fall, FootSurface surface, const vec3 feet,
                    const vec3 right);

// Every take of every surface in turn, one each half second of `time`, for listening to them in
// an audio dump (--footstep-say).
void footsteps_say(Footsteps* f, double time, const vec3 feet);

#endif // _SILENT_FOOTSTEPS_H_
