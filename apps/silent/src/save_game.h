#ifndef _SILENT_SAVE_GAME_H_
#define _SILENT_SAVE_GAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "cetra/game/game.h"

#include "backpack.h"
#include "basement.h"
#include "cat.h"
#include "cat_brain.h"
#include "door.h"
#include "fridge_map.h"
#include "lights.h"
#include "map_screen.h"
#include "player.h"
#include "town_map.h"

/*
 * A game's autosave (spec 13.49): one file per playthrough, a slot of the engine's save system
 * under the per-user directory, overwritten whenever the player takes something, finds a place,
 * goes into another room or quits. There are no manual saves.
 *
 * What the file keeps is the player's body and look, the bag and what is in it, the places found
 * and the marks inked, each door's swing, the flashlight, the basement's draft, and where the cat
 * is with what it wants. Every one is copied into one struct before a write and pushed back into
 * its module after a read, so the module that owns a thing rebuilds what follows from it -- a
 * door's body, the bag's grid, the cat's posture. Keys are each thing's own id, never an index a
 * new item would renumber. A crouch is not kept: a load stands the player up where there is room.
 * Everything else -- the clock, the lamps, the rain -- follows from the sim clock, which the
 * engine's own section keeps.
 */

#define SAVE_GAME_SLOTS 8 // games kept at once, one slot each
#define SAVE_GAME_DOORS 8

// The world a save reads and writes, borrowed. A door not hung is left out; the cat and its mind
// may have no entity and no brain, and are then left out.
typedef struct SaveWorld {
    Game* game;
    Player* player;
    Backpack* backpack;
    FridgeMap* fridge_map;
    Finds* finds;
    MapScreen* map_screen;
    Lights* lights;
    Basement* basement;
    Cat* cat;
    CatMind* mind;
    Door* doors[SAVE_GAME_DOORS];
    int door_count;
} SaveWorld;

// Why a save is due: a room only once a while has passed since the last write.
typedef enum { SAVE_TOOK, SAVE_FOUND, SAVE_ROOM, SAVE_QUIT } SaveMoment;

// One slot as the load list shows it.
typedef struct SaveSlot {
    char name[16];   // its file's name, "silent-N"
    double saved_at; // seconds since the epoch
    double played;   // seconds of play
    vec3 feet;       // where the player stood
} SaveSlot;

// The save's tables over `world`, which must outlive it. False with nothing to save into.
bool save_game_start(const SaveWorld* world);
void save_game_free(void);

// Every slot holding a game, newest first, into `out`, SAVE_GAME_SLOTS long: how many.
int save_game_list(SaveSlot* out);
// The lowest slot not in use, into `out`: false when every one is.
bool save_game_free_slot(SaveSlot* out);
// The game in `slot` over the world as it was built: false, said, when it could not be read.
bool save_game_load(const char* slot);
// `slot`'s file gone.
bool save_game_delete(const char* slot);

// From now on the game saves itself into `slot`; NULL and it never does.
void save_game_begin(const char* slot);
// The slot the game saves itself into, or NULL.
const char* save_game_slot(void);
// Something worth keeping happened: written at the next save_game_frame, and at once on a quit.
void save_game_moment(SaveMoment moment);
// Per frame while the game is played: the play clock, the room the `feet` are in, and a save due.
void save_game_frame(const vec3 feet, double dt);

#endif // _SILENT_SAVE_GAME_H_
