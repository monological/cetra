#ifndef _SILENT_SETTINGS_MENU_H_
#define _SILENT_SETTINGS_MENU_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/ui.h"
#include "cetra/game/audio.h"
#include "cetra/game/settings.h"

/*
 * The settings (spec 13.50): what a player chose, kept in a file of silent's own beside the saves
 * -- the volume, the window's mode and vsync, how fast the view turns and which way is up for it,
 * and the field of view. One screen over whatever is on top, hiding it, its rows in the menus'
 * look, everything applied the moment it is chosen and the file written when the screen goes.
 */

#define SETTINGS_FOV  68.0f // degrees, what the view is when the file says nothing
#define SETTINGS_ROWS 6

typedef struct SettingsMenu SettingsMenu;

// A row's own: the menu, for the UI its track is asked of, and how a slider's value reads -- its
// value times `scale` through `format` -- or NULL for a toggle or a selector.
typedef struct SettingsRow {
    const SettingsMenu* menu;
    const char* format;
    float scale;
} SettingsRow;

struct SettingsMenu {
    GameSettings settings; // what is chosen, which the rows write
    char path[1024];       // where it is kept; empty for nowhere
    bool mute;             // applied with the volume at 0, whatever it says
    float fov;             // applied with this field of view, whatever it says; 0 for its own
    bool dirty;            // changed since it was last written
    Engine* engine;
    AudioSystem* audio;
    UISystem* ui; // the HUD's, borrowed
    UIScreen* screen;
    UIElement* first; // the row it opens on
    SettingsRow rows[SETTINGS_ROWS];
};

// The file read, defaults where it says nothing, with a volume held at 0 for `mute` and a field of
// view for `fov` above 0 to be applied over it: before the engine is made, so the window opens as
// chosen.
void settings_menu_load(SettingsMenu* sm, bool mute, float fov);
// Its screen in `ui`, which may be NULL for none, applying to `engine` and `audio`, either of which
// may be NULL. Nothing is applied yet.
void settings_menu_start(SettingsMenu* sm, UISystem* ui, Engine* engine, AudioSystem* audio);
// What is chosen, on the audio, the window and the engine's camera and its installed rig: once
// started, and again whenever another rig is installed.
void settings_menu_apply(const SettingsMenu* sm);
// A menu item in `parent` that opens the screen.
UIElement* settings_menu_item(UIElement* parent, float size, SettingsMenu* sm);
// Up over whatever is on top, hiding it.
void settings_menu_show(SettingsMenu* sm);
// Whether it is on top.
bool settings_menu_open(const SettingsMenu* sm);
// The file written if anything changed since it last was.
void settings_menu_save(SettingsMenu* sm);
// After the UI's pass each frame: once the screen has gone, the file written.
void settings_menu_frame(SettingsMenu* sm);

#endif // _SILENT_SETTINGS_MENU_H_
