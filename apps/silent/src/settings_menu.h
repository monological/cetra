#ifndef _SILENT_SETTINGS_MENU_H_
#define _SILENT_SETTINGS_MENU_H_

#include <stdbool.h>

#include "cetra/camera_rig.h"
#include "cetra/engine.h"
#include "cetra/ui.h"
#include "cetra/game/audio.h"
#include "cetra/game/settings.h"

/*
 * The settings (spec 13.50): what a player chose, kept in a file of silent's own beside the saves
 * and applied the moment it is chosen -- the volume, the window's mode and vsync, how fast the view
 * turns and which way is up for it, and the field of view. One screen, opened from the title or
 * the pause menu over whichever it came from, its rows in the menus' look; the file is written
 * when the screen goes.
 */

#define SETTINGS_FOV 68.0f // degrees, what the view is when the file says nothing

typedef struct SettingsMenu {
    GameSettings settings; // what is chosen, which the rows write
    char path[1024];       // where it is kept; empty for nowhere
    bool mute;             // the volume held at 0 whatever it says
    UISystem* ui;          // the HUD's, borrowed
    UIScreen* screen;
    UIElement* first; // the row it opens on
    Engine* engine;
    AudioSystem* audio;
    CameraRig* look; // the player's, whose turn and up the look rows set
    bool dirty;      // changed since it was last written
} SettingsMenu;

// The file read, defaults where it says nothing: before the engine is made, so the window opens
// as chosen.
void settings_menu_load(SettingsMenu* sm);
// Its screen in `ui`, which may be NULL for none, and what is chosen applied to the engine, the
// audio and `look`, any of which may be NULL.
void settings_menu_start(SettingsMenu* sm, UISystem* ui, Engine* engine, AudioSystem* audio,
                         CameraRig* look);
// Up over whatever is on top, hiding it.
void settings_menu_show(SettingsMenu* sm);
// Whether it is on top.
bool settings_menu_open(const SettingsMenu* sm);
// After the UI's pass each frame: once the screen has gone, the file written if anything changed.
void settings_menu_frame(SettingsMenu* sm);

#endif // _SILENT_SETTINGS_MENU_H_
