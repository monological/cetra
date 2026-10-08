#ifndef _SILENT_BASEMENT_H_
#define _SILENT_BASEMENT_H_

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"

#include "door.h"
#include "home.h"
#include "kit.h"
#include "sounds.h"

/*
 * The basement under the player's house (spec 13.31): the whole footprint dug out below the
 * ground floor, its walls the house's foundation, its ceiling the floor's own framing on a beam
 * down the middle, reached by a steep flight from the door at the hall's end.
 */

// Its walls' inner faces.
#define CELLAR_X0 (HOUSE_X0 + 0.5f * EXT_WALL)
#define CELLAR_X1 (HOUSE_X1 - 0.5f * EXT_WALL)
#define CELLAR_Z0 (HOUSE_FRONT_Z + 0.5f * EXT_WALL)
#define CELLAR_Z1 (HOUSE_BACK_Z - 0.5f * EXT_WALL)
// And their outer ones, which is where the yard is cut away round it.
#define DIG_X0 (HOUSE_X0 - 0.5f * EXT_WALL)
#define DIG_X1 (HOUSE_X1 + 0.5f * EXT_WALL)
#define DIG_Z0 (HOUSE_FRONT_Z - 0.5f * EXT_WALL)
#define DIG_Z1 (HOUSE_BACK_Z + 0.5f * EXT_WALL)

/*
 * The bare bulb hanging in the stairwell, which is what the door opens on: lit, set swinging by
 * the draft the first time the door opens, and now and then dimming on a failing supply. And the
 * tap over the laundry tub, dripping into the water standing in it.
 */
typedef struct Basement {
    SceneNode* bulb; // the cord, the socket and the glass, turned about the rose
    Material* glass; // its own, driven with the light
    Light* light;    // cached, and redrawn each frame while the swing is wide
    double drafted;  // when the door first opened, or -1 before
    unsigned int seed;

    AudioSystem* audio;
    Sound* drip; // decoded once and played as voices; NULL without audio
    double next_drip;
    KitRng drips; // the gaps between them
} Basement;

// The foundation, the slab, the framing overhead, the beam and its posts, the stair and its
// walls, what is kept down there and the damp standing on its floor, placed from `seed`, and the
// bulb's rose on the ceiling.
void basement_build(Kit* kit, unsigned int seed);

// The bulb, a node of its own with its light, and the drip from `audio`, which may be NULL.
void basement_start(Basement* b, Engine* engine, Scene* scene, AudioSystem* audio,
                    unsigned int seed);

// Per frame, before the frame draws: the swing, set going the first time `door` opens (NULL when
// it was not hung), the dimming, and the drip, heard from `eye` as `sounds` says.
void basement_update(Basement* b, const Door* door, const Sounds* sounds, const vec3 eye,
                     double time);

#endif // _SILENT_BASEMENT_H_
