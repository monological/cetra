#ifndef _LOADING_SCREEN_H_
#define _LOADING_SCREEN_H_

#include <stdbool.h>

#include "engine.h"

/*
 * The engine's loading screen (spec 13.34): CETRA over ENGINE as a 1970s station ident on a worn
 * tape, played on an old television -- CETRA's letters turn in one after another to face the eye,
 * ENGINE lights beneath, a light runs once along a striped rule and sparkles at its end, the tape
 * glitches, and a LOADING sign waits until the game is ready. Shown, it covers the window from
 * then until it is hidden and has switched off. Engine frames draw it in place of their picture;
 * before engine_run, an app's init draws it between its steps.
 *
 * Its look is a palette among LOADING_PALETTE_COUNT, `Engine.loading_palette`.
 */

typedef enum LoadingPalette {
    LOADING_PALETTE_SUNSET,    // cream, mustard, orange, red and brown
    LOADING_PALETTE_HARVEST,   // cream, harvest gold, burnt orange, avocado and chocolate
    LOADING_PALETTE_PHOSPHOR,  // a green tube
    LOADING_PALETTE_BROADCAST, // blues and violet
    LOADING_PALETTE_COUNT
} LoadingPalette;

// From now on the window shows it, from the start of its ident. Showing it again while it is
// shown starts the ident over.
void engine_show_loading_screen(Engine* engine);

// Draw it now and answer the window's events: for an app's init, before engine_run, which draws
// it with every frame and between the long pieces of one. Outside a frame only, since an event
// may resize the window. It draws at most once a refresh, so call it as often as is convenient.
// Headless it does nothing: there, only frames draw it, so the frame decides what it shows. A NULL
// engine is no screen, and nothing: code with no engine to draw with may pass one.
void engine_draw_loading_screen(const Engine* engine);

// The game is ready: once its ident has played, the screen trades its LOADING sign for PLAY and
// waits, still up, for the app to hide it -- at a press it chooses, since the engine does not know
// the game's controls. An app with no prompt to give hides it instead.
void engine_loading_screen_ready(Engine* engine);

// Lift it: once its ident has played, the set switches off and the window shows the frame again.
void engine_hide_loading_screen(Engine* engine);

// True from show until the switch-off has finished.
bool engine_loading_screen_shown(const Engine* engine);

// True while PLAY is on the screen and it has not been hidden: when a press means "play".
bool engine_loading_screen_prompting(const Engine* engine);

// The longest wall-clock time, in seconds, between two chances the screen had to draw -- a call to
// engine_draw_loading_screen, a draw between the pieces of a long frame, or a frame -- since it was
// shown or this was last asked, which starts the count again; 0 when it is not shown. Counted
// headless too, where only frames draw it, so a headless run says how long a window would freeze.
double engine_loading_screen_take_longest_wait(Engine* engine);

#endif
