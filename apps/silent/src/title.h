#ifndef _SILENT_TITLE_H_
#define _SILENT_TITLE_H_

#include <stdbool.h>

#include "cetra/camera_rig.h"
#include "cetra/ui.h"
#include "cetra/game/game.h"

#include "save_game.h"
#include "settings_menu.h"

/*
 * The title (spec 13.49): once the game has loaded, the view down the street in the fog from the
 * far pavement, and over it Continue, New Game, Load Game, Settings and Exit to Desktop (spec
 * 13.50). Load Game lists the games kept, newest first, where each was left and how long it has
 * been played, and one can be deleted from the list after a yes. Three modal screens in the HUD's
 * UI system, the list and the question each hiding what it was opened over, and a camera rig of
 * the title's own, installed on the engine from the start, so the player's is never posed for it.
 */

typedef enum { TITLE_WAITING, TITLE_NEW, TITLE_LOAD } TitleChoice;

typedef struct Title {
    UISystem* ui; // the HUD's, borrowed
    CameraRig* rig;
    UIScreen* main;
    UIScreen* games;
    UIScreen* confirm;
    UIElement* resume; // Continue
    UIElement* fresh;  // New Game
    UIElement* load;   // Load Game
    UIElement* note;   // what New Game cannot do, when it cannot
    UIElement* rows[SAVE_GAME_SLOTS];
    UIElement* asked; // the game the question is about
    UIElement* yes;
    UIElement* no;
    SaveSlot slots[SAVE_GAME_SLOTS];
    int count;
    TitleChoice choice; // what was chosen, its screens taken down; TITLE_WAITING until then
    SaveSlot chosen;    // the slot a new game takes, or the one loaded
} Title;

// Its screens in `ui`, which may be NULL for none, opening `settings` and quitting `game`, and its
// rig on the game's engine, so the view loads round the title's eye; nothing shown yet.
void title_start(Title* t, UISystem* ui, Game* game, SettingsMenu* settings);
// Up over the street, the games kept read again, on its first item: false with no UI to show.
bool title_show(Title* t);
// Over it, the screen called `name` -- "games", or "delete" asking about the newest game -- as
// though asked for: what a headless run opens to photograph it. NULL or another name does nothing.
void title_open_screen(Title* t, const char* name);
// Whether its own first screen is on top, which back does not take down.
bool title_on_main(const Title* t);
// The game the list is on, asked about: what the delete key does there.
void title_ask_delete(Title* t);
// Back over the street after a game could not be loaded, saying so.
void title_refused(Title* t);
void title_free(Title* t);

#endif // _SILENT_TITLE_H_
