#ifndef _SILENT_FOOTSTEPS_H_
#define _SILENT_FOOTSTEPS_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "cetra/gait.h"
#include "cetra/game/audio.h"
#include "cetra/game/physics.h"

#include "kit.h"

/*
 * The player's footsteps (spec 13.47): a recording of what the feet are on, played where a foot
 * comes down, each time the gait lets one fall.
 *
 * What the feet are on is what they stand on: the material of the kit's body a ray down from them
 * finds. Three things are not bodies of a material and are asked otherwise: a rug, which is drawn
 * and not collided, by the rugs' own tests; the lake, by the feet being in it; and the open
 * ground, one body of none, by what is drawn there -- the drive, the track, or the land's own
 * ground. Out in the rain, under open sky, hard ground and grass are wet and sound it.
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
    AudioSystem* audio;          // NULL: silent
    PhysicsWorld* physics;       // what the ray down asks
    const KitSurfaces* surfaces; // and what the body it finds is made of
    Sound* takes[FOOT_SURFACES][FOOTSTEP_TAKES];
    int count[FOOT_SURFACES];
    int last[FOOT_SURFACES]; // the take played last, which is not played next; -1 before any
    unsigned int rng;
    int said; // takes played by footsteps_say
    bool trace;
} Footsteps;

// The takes there are of each surface, from `audio`, which may be NULL.
void footsteps_start(Footsteps* f, AudioSystem* audio, PhysicsWorld* physics,
                     const KitSurfaces* surfaces, uint32_t seed);

// A foot came down: `feet` is where the feet are in the world, `yaw` the body's heading (radians
// about +Y, facing (sin, 0, cos)), `wetness` the rain's film on ground under open sky, 0 under
// a roof or in no rain, and `crouched` whether the body is bent low, setting each foot down softly.
void footsteps_fall(Footsteps* f, const GaitFootfall* fall, const vec3 feet, float yaw,
                    float wetness, bool crouched);

// Every take of every surface in turn at `feet`, one each half second of `time`, for listening to
// them in an audio dump.
void footsteps_say(Footsteps* f, double time, const vec3 feet);

#endif // _SILENT_FOOTSTEPS_H_
