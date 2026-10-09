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
    const float* fg; // its words' colour
    float alpha;     // what the plate and its words are drawn at
} HudLine;

typedef struct Hud {
    UISystem* ui;
    Font* font; // the face every screen in it is set in, the font pool's
    UIScreen* screen;
    HudLine prompt, thought;
    float thought_age; // seconds since the thought was had
    const char* hint;  // the prompt while no other is given, borrowed
    float hint_left;   // seconds it has left
} Hud;

// False, with the reason printed, if the font or the UI cannot be made; the game runs on without
// words.
bool hud_start(Hud* hud, Engine* engine);
// Show `text` as the prompt, or the hint, or nothing, for NULL. Cheap to call every frame with
// the same text.
void hud_prompt(Hud* hud, const char* text);
// `text` as the prompt for `seconds` while no other is given, from the next hud_prompt.
void hud_hint(Hud* hud, const char* text, float seconds);
// Think `text`, in place of any thought on screen; the same thought again stays up.
void hud_think(Hud* hud, const char* text);
// Per frame: the thought's fade and the hint's time, and the lines placed in a window
// `height` points tall.
void hud_update(Hud* hud, float dt, float height);
void hud_free(Hud* hud);

#endif // _SILENT_HUD_H_
