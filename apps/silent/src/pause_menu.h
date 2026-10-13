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
    bool to_title; // Quit to Title chosen, and not yet taken
} PauseMenu;

// Its screen in `ui`, which may be NULL for none, opening `settings`, offering Quit to Title on a
// `titled` run, and quitting `game`; nothing shown yet.
void pause_menu_start(PauseMenu* pm, UISystem* ui, Game* game, SettingsMenu* settings, bool titled);
// Up over the game, on Resume.
void pause_menu_show(PauseMenu* pm);
// Whether Quit to Title was chosen, the menu taken down: true once for each choice. Until it is
// taken the menu stays up, so the game under it stays frozen.
bool pause_menu_take_to_title(PauseMenu* pm);
// Whether it is up, on top or under the settings.
bool pause_menu_up(const PauseMenu* pm);

#endif // _SILENT_PAUSE_MENU_H_
