#ifndef _SILENT_BACKPACK_H_
#define _SILENT_BACKPACK_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

/*
 * The player's backpack (spec 13.40): an old canvas daypack lying on the bed in the home's
 * bedroom, with a flashlight in it. Taking it takes what is in it, and the flashlight works from
 * then on. Each thing in it is a model of its own on no scene graph, which the backpack's screen
 * shows on its own.
 *
 * What is carried lies in a grid of cells, each thing across its own footprint, and the GAME packs
 * it: a thing goes where it first fits, in the order it was had; when one will not fit, everything
 * is packed again largest first; when even that fails, the grid takes another row. Nobody ever
 * arranges the bag, and it is never full.
 */

typedef enum { ITEM_NONE = -1, ITEM_FLASHLIGHT, ITEM_COUNT } ItemId;

#define BAG_COLS     8
#define BAG_MIN_ROWS 4
#define BAG_MAX_ROWS 32
#define BAG_SPARE    2 // empty rows always shown under what is carried

typedef struct ItemSpec {
    const char* id;   // as a command line names it
    const char* name; // as the backpack's screen shows it
    const char* line; // what the player knows about it
    int cells[2];     // its footprint: across, down
} ItemSpec;

extern const ItemSpec ITEMS[ITEM_COUNT];

// The item a command line names, or ITEM_NONE.
ItemId item_by_id(const char* id);

typedef struct Backpack {
    bool taken;                    // the player has it
    SceneNode* bag;                // lying on the bed until then
    vec3 at;                       // the middle of it, which the player reaches for
    SceneNode* models[ITEM_COUNT]; // each thing in it alone, for the backpack's screen
    // What is carried, in the order it was had, and where each lies in the grid.
    ItemId held[ITEM_COUNT];
    int held_count;
    int cell[ITEM_COUNT][2]; // its footprint's first column and row
    int rows;                // the grid's, spare rows included
} Backpack;

// The bag on the bed, or nothing there when it is already `taken`, and the models of what is in
// it.
void backpack_build(Backpack* bp, Engine* engine, Scene* scene, bool taken);
// Off the bed: the player has it, and what is in it.
void backpack_take(Backpack* bp);
// Whether the player has `item`.
bool backpack_holds(const Backpack* bp, ItemId item);
void backpack_free(Backpack* bp);

#endif // _SILENT_BACKPACK_H_
