#ifndef _SILENT_BASEMENT_H_
#define _SILENT_BASEMENT_H_

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

// The foundation, the slab, the framing overhead and the beam and its posts.
void basement_build(Kit* kit);

#endif // _SILENT_BASEMENT_H_
