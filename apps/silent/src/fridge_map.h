#ifndef _SILENT_FRIDGE_MAP_H_
#define _SILENT_FRIDGE_MAP_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The town map on the fridge (spec 13.43): a folded street map of Pale Ridge hanging from a magnet
 * on the freezer door, half open -- its printed cover held flat to the door and the panel under it
 * sagging out into the room. It is meant to be the thing on the door the eye goes to: the only
 * glossy, brightly printed thing among faded snapshots, and the only one with a thickness and a
 * silhouette rather than a flat card. Taking it puts it in the backpack.
 */

typedef struct FridgeMap {
    SceneNode* node; // hanging on the freezer door; NULL once it is taken
    vec3 at;         // the middle of its cover, which the player reaches for
} FridgeMap;

// The map on the freezer door, or nothing there when it is already `taken`.
void fridge_map_build(FridgeMap* fm, Engine* engine, Scene* scene, bool taken);
// Off the door.
void fridge_map_take(FridgeMap* fm);
// The magnet it hangs from, in the frame it hangs in; the magnet stays when it is taken.
void fridge_map_magnet(Kit* kit, const KitFrame* f);

// The folded map closed, its cover facing +d and centred on f's origin.
void fridge_map_closed(Kit* kit, const KitFrame* f);

#endif // _SILENT_FRIDGE_MAP_H_
