#ifndef _SILENT_PAUSE_MENU_H_
#define _SILENT_PAUSE_MENU_H_

#include <stdbool.h>

#include "cetra/ui.h"
#include "cetra/game/game.h"

#include "settings_menu.h"

/*
 * The pause menu (spec 13.50): a modal screen in the HUD's UI system, in the menus' look, holding
 * Resume, which takes it down as its back does, Settings, which opens the settings over it, Quit to
 * Title on a run that has one, and Exit to Desktop.
 */

typedef struct PauseMenu {
    UISystem* ui; // the HUD's, borrowed
    UIScreen* screen;
    UIElement* resume;
} PauseMenu;

// Its screen in `ui`, which may be NULL for none, opening `settings`, going back to the title by
// `to_title` -- NULL leaves the item out -- and quitting `game`; nothing shown yet.
void pause_menu_start(PauseMenu* pm, UISystem* ui, Game* game, SettingsMenu* settings,
                      UIActionFn to_title);
// Up over the game, on Resume.
void pause_menu_show(PauseMenu* pm);
// Whether it is up, on top or under the settings.
bool pause_menu_up(const PauseMenu* pm);

#endif // _SILENT_PAUSE_MENU_H_
