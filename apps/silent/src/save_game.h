#ifndef _SILENT_SAVE_GAME_H_
#define _SILENT_SAVE_GAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "cetra/game/game.h"

#include "backpack.h"
#include "basement.h"
#include "boat.h"
#include "cat.h"
#include "cat_brain.h"
#include "door.h"
#include "inspect.h"
#include "lights.h"
#include "map_screen.h"
#include "player.h"
#include "town_map.h"

typedef struct Story Story;

/*
 * A game's autosave (spec 13.49): one file per playthrough, a slot of the engine's save system
 * under the per-user directory, overwritten whenever the player takes something, finds a place,
 * goes into another room or quits. There are no manual saves.
 *
 * What the file keeps is the player's body and look, the bag and what is in it, the places found
 * and the marks inked, each door's swing, the flashlight, the basement's draft, how far the house's
 * story has gone, where the boat is and whether the player is in it, and where the cat is with what
 * it wants. A value a module holds as a field is
 * read and written in place; the few a module holds only in some other shape -- the order the bag's
 * things were had, the marks written whole, the cat's place -- are worked out before a write and
 * put back after a read. Then the module that owns a thing rebuilds what follows from it: a door's
 * body, the bag's grid, the cat's posture. Keys are each thing's own id, never an index a new item
 * would renumber. A crouch is not kept: a load stands the player up where there is room. Everything
 * else -- the clock, the lamps, the rain -- follows from the sim clock, which the engine's own
 * section keeps.
 */

#define SAVE_GAME_SLOTS 8 // games kept at once, one slot each
#define SAVE_GAME_DOORS 8

// The world a save reads and writes, borrowed. A door not hung is left out; the cat and its mind
// may have no entity and no brain, and are then left out.
typedef struct SaveWorld {
    Game* game;
    Player* player;
    Backpack* backpack;
    Inspect* inspect;
    Finds* finds;
    MapScreen* map_screen;
    Lights* lights;
    Basement* basement;
    Story* story;
    Boat* boat; // NULL, or one with no model, is left out
    Cat* cat;
    CatMind* mind;
    Door* doors[SAVE_GAME_DOORS];
    int door_count;
} SaveWorld;

// One slot as the load list shows it.
typedef struct SaveSlot {
    char name[16];     // its file's name, "silent-N"
    double saved_at;   // seconds since the epoch
    double played;     // seconds of play
    const char* where; // the room the player was in, as a player reads it
} SaveSlot;

// The save's tables over `world`, which must outlive it. False with nothing to save into.
bool save_game_start(const SaveWorld* world);
void save_game_free(void);

// Every slot holding a game, newest first, into `out`, SAVE_GAME_SLOTS long: how many.
int save_game_list(SaveSlot* out);
// The lowest slot no game is in, into `out`: false when every one holds one.
bool save_game_unused_slot(SaveSlot* out);
// The game in `slot` over the world as it was built: false, said, when it could not be read.
bool save_game_load(const char* slot);
// `slot`'s file gone.
bool save_game_delete(const char* slot);

// The world as it is now kept as its start (spec 13.50), in a slot no list shows: false, said,
// when it could not be.
bool save_game_keep_start(void);
// The world put back to the start kept, as save_game_load puts a game: false, said, when it could
// not be read.
bool save_game_load_start(void);

// From now on the game saves itself into `slot`; NULL, or a slot with nowhere to go, and it never
// does.
void save_game_begin(const char* slot);
// Something worth keeping happened: written at the next save_game_frame.
void save_game_due(void);
// The game is ending: written now.
void save_game_quit(void);
// Per frame while the game is played: the play clock, the room the `feet` are in, the story's
// stage, and a save due.
// True when it wrote one.
bool save_game_frame(const vec3 feet, double dt);

#endif // _SILENT_SAVE_GAME_H_
