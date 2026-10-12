#ifndef _SILENT_TITLE_H_
#define _SILENT_TITLE_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/camera_rig.h"
#include "cetra/engine.h"
#include "cetra/ui.h"

#include "save_game.h"

/*
 * The title (spec 13.49): once the game has loaded, the view down the street in the fog from the
 * far pavement, and over it Continue, New Game and Load Game. Load Game lists the games kept,
 * newest first, where each was left and how long it has been played, and one can be deleted from
 * the list after a yes. Three modal screens in the HUD's UI system, and a camera rig of the
 * title's own, installed on the engine while it shows, so the player's is never posed for it.
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
    char labels[SAVE_GAME_SLOTS][96];
    int count;
    int doomed; // the row a delete is asking about
    bool up;    // shown and nothing chosen yet: one of its screens is always on top
    TitleChoice choice;
    SaveSlot chosen; // the slot a new game takes, or the one loaded
} Title;

// Its screens in `ui`, which may be NULL for none, and its rig; nothing shown yet.
void title_start(Title* t, UISystem* ui);
// Its eye and what it looks at, in the world.
void title_view(vec3 eye, vec3 target);
// The rig on the engine, for the view to load round while the loading screen is up.
void title_frame(Title* t, Engine* engine);
// Up over the street, the games kept read again, on its first item.
void title_show(Title* t);
// Over it, the screen called `name` -- "games", or "delete" asking about the newest game -- as
// though asked for: what a headless run opens to photograph it. NULL or another name does nothing.
void title_open_screen(Title* t, const char* name);
// Whether one of its screens is on top.
bool title_open(const Title* t);
// Whether `back` should take its top screen down: the list and the question, never the title.
bool title_backs(const Title* t);
// After the UI's pass each frame: the title back when the list has gone, and the frame's delete
// press, which on a row of the list asks first.
void title_input(Title* t, bool delete_pressed);
// What was chosen, once a choice is made, the screens taken down; TITLE_WAITING until then.
TitleChoice title_choice(const Title* t);
// Back over the street after a game could not be loaded, saying so.
void title_refused(Title* t);
void title_free(Title* t);

#endif // _SILENT_TITLE_H_
