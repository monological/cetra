#ifndef _SILENT_BASEMENT_H_
#define _SILENT_BASEMENT_H_

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/scene.h"

#include "door.h"
#include "home.h"
#include "kit.h"

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
 * the draft the first time the door opens, and now and then dimming on a failing supply.
 */
typedef struct Basement {
    SceneNode* bulb; // the cord, the socket and the glass, turned about the rose
    Material* glass; // its own, driven with the light
    Light* light;    // cached, and redrawn each frame while the swing is wide
    double drafted;  // when the door first opened, or -1 before
    unsigned int seed;
} Basement;

// The foundation, the slab, the framing overhead, the beam and its posts, the stair and its
// walls, and the bulb's rose on the ceiling.
void basement_build(Kit* kit);

// The bulb: a node of its own with its light.
void basement_start(Basement* b, Engine* engine, Scene* scene, unsigned int seed);

// Per frame, before the frame draws: the swing, set going the first time `door` opens (NULL when
// it was not hung), and the dimming.
void basement_update(Basement* b, const Door* door, double time);

#endif // _SILENT_BASEMENT_H_
