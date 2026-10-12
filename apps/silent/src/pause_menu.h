#ifndef _SILENT_PAUSE_MENU_H_
#define _SILENT_PAUSE_MENU_H_

#include <stdbool.h>

#include "cetra/ui.h"

#include "settings_menu.h"

/*
 * The pause menu (spec 13.50): Escape or pad Start while the game is played brings it up over the
 * world, which stands still under it, the rain and all. A modal screen in the HUD's UI system, in
 * the title's look. Resume, or its own back, takes it down, and Settings opens the settings over
 * it; what else is chosen is for the game to act on.
 */

typedef enum { PAUSE_STAYING, PAUSE_EXIT } PauseChoice;

typedef struct PauseMenu {
    UISystem* ui;           // the HUD's, borrowed
    SettingsMenu* settings; // borrowed
    UIScreen* screen;
    UIElement* resume;
    PauseChoice choice; // chosen and not yet acted on; PAUSE_STAYING until then
} PauseMenu;

// Its screen in `ui`, which may be NULL for none, opening `settings`; nothing shown yet.
void pause_menu_start(PauseMenu* pm, UISystem* ui, SettingsMenu* settings);
// Up over the game, on Resume.
void pause_menu_show(PauseMenu* pm);
// Whether it is on top: the game stands still while it is.
bool pause_menu_open(const PauseMenu* pm);
// What was chosen, once; PAUSE_STAYING when nothing was.
PauseChoice pause_menu_take(PauseMenu* pm);

#endif // _SILENT_PAUSE_MENU_H_
