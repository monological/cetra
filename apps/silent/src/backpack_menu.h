#ifndef _SILENT_BACKPACK_MENU_H_
#define _SILENT_BACKPACK_MENU_H_

#include <stdbool.h>

#include "cetra/text.h"
#include "cetra/ui.h"

#include "backpack.h"
#include "item_view.h"

/*
 * The backpack's screen (spec 13.40), opened by Tab once the player has it, after Deus Ex: Human
 * Revolution's inventory: one near-black pane in amber over the world dimmed behind it, a title,
 * the bag's GRID on the left with each thing carried lying across its own footprint as a picture
 * of itself, and on the right the one chosen -- by a click, or the arrows and Enter -- turning
 * over its name and what the player knows of it. The game packs the grid (backpack.h); nothing
 * here moves anything. A modal screen in the HUD's UI system; Tab or Escape closes it, and the
 * world goes on behind it.
 */
typedef struct BackpackMenu {
    UISystem* ui;         // the HUD's, borrowed
    Font* font;           // the HUD's face, which the line about an item is wrapped in
    const Backpack* pack; // borrowed
    UIScreen* screen;
    UIElement* pane;
    UIElement* grid;
    UIElement* detail;
    UIElement* view;
    UIElement* name;
    UIElement* line;
    ItemStage stage;
    ItemView icons[ITEM_COUNT]; // each item's picture in the grid, drawn once a size
    ItemView turntable;         // the chosen one's, every frame
    ItemId chosen;              // or ITEM_NONE while nothing is
    ItemId hovered;             // under the pointer
    ItemId cursor;              // what the arrows are on
    float angle;                // how far the chosen one has turned
    float scale;                // framebuffer pixels to a window point
    float wrapped_at;           // the width the line about the chosen item was wrapped to
} BackpackMenu;

// False if there is no UI to show it in, or its pictures will not build, which says why; Tab then
// shows nothing.
bool backpack_menu_start(BackpackMenu* menu, UISystem* ui, Font* font, const Backpack* pack);
// Whether it is the screen on top.
bool backpack_menu_open(const BackpackMenu* menu);
// Up, with nothing chosen and the arrows on the first thing in the grid. It goes by the UI's own
// back.
void backpack_menu_show(BackpackMenu* menu);
void backpack_menu_choose(BackpackMenu* menu, ItemId item);
// The pane sized to a window `width` x `height` points, and the window's pixels to a point: each
// frame before the UI's pass, which hit-tests what this lays out.
void backpack_menu_layout(BackpackMenu* menu, float width, float height, float scale);
// The frame's input while it is open: the pointer over the grid, a click, the arrows and Enter.
void backpack_menu_input(BackpackMenu* menu, const UIInput* in);
// Per frame: the chosen item's turn.
void backpack_menu_update(BackpackMenu* menu, float dt);
void backpack_menu_free(BackpackMenu* menu);

#endif // _SILENT_BACKPACK_MENU_H_
