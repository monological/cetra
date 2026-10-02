#ifndef _SILENT_PROMPT_H_
#define _SILENT_PROMPT_H_

#include <stdbool.h>

#include "cetra/engine.h"
#include "cetra/ui.h"

/*
 * The one line of text the game ever shows (spec 13.13): what pressing the action key would
 * do here -- "E  Open door" -- low in the middle of the screen while there is something to
 * do, and gone when there is not. A non-modal screen of the engine's UI layer, drawn after the
 * tonemap so the grade and the fog never touch it.
 */
typedef struct Prompt {
    const Engine* engine; // borrowed: the window it is placed in
    UISystem* ui;
    UIScreen* screen;
    UIElement* label;
    bool shown;
    char text[64];
} Prompt;

// False, with the reason printed, if the font or the UI cannot be made; the game runs on
// without prompts.
bool prompt_start(Prompt* prompt, Engine* engine);
// Show `text`, or nothing for NULL. Cheap to call every frame with the same text.
void prompt_show(Prompt* prompt, const char* text);
void prompt_free(Prompt* prompt);

#endif // _SILENT_PROMPT_H_
