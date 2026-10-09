#ifndef _SILENT_HUD_H_
#define _SILENT_HUD_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/ui.h"

/*
 * What the game says on screen (specs 13.13 and 13.40), through the engine's UI layer after the
 * tonemap, so the grade and the fog never touch it. Two lines low in the middle: the PROMPT, what
 * pressing the action key would do here -- "E   Open door" -- while there is something to do,
 * and over it a THOUGHT, something the player thinks, which fades in, stays a few seconds and
 * fades out. Both are on one non-modal screen that never leaves the stack. The engine has one
 * overlay, so every screen the game shows -- the backpack's (backpack.h) -- is pushed over it in
 * this one UI system.
 */
typedef struct HudLine {
    UIElement* plate;
    UIElement* label;
    char text[128];
    float alpha; // what the plate and its words are drawn at
} HudLine;

typedef struct Hud {
    const Engine* engine; // borrowed: the window it is placed in
    UISystem* ui;
    UIScreen* screen;
    HudLine prompt, thought;
    float thought_age; // seconds since the thought was had
} Hud;

// False, with the reason printed, if the font or the UI cannot be made; the game runs on without
// words.
bool hud_start(Hud* hud, Engine* engine);
// Show `text` as the prompt, or nothing for NULL. Cheap to call every frame with the same text.
void hud_prompt(Hud* hud, const char* text);
// Think `text`, in place of any thought on screen.
void hud_think(Hud* hud, const char* text);
// Per frame: the thought's fade, and the lines placed in the window as it is now.
void hud_update(Hud* hud, float dt);
void hud_free(Hud* hud);

#endif // _SILENT_HUD_H_
