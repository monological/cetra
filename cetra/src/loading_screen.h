#ifndef _LOADING_SCREEN_H_
#define _LOADING_SCREEN_H_

#include <stdbool.h>

#include "engine.h"

/*
 * The engine's loading screen (spec 13.34): CETRA over ENGINE as a 1970s station ident on a worn
 * tape, played on an old television -- each letter of CETRA flips in, ENGINE lights beneath, and a
 * light sweeps along a striped rule while the game loads. Shown, it covers the window from then
 * until it is hidden and has switched off. Engine frames draw it in place of their picture; before
 * engine_run, an app's init draws it between its steps.
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

// Draw it now and answer the window: for an app's init, before engine_run, which draws it with
// every frame. It draws at most once a refresh, so call it as often as is convenient. Headless it
// does nothing: there, only frames draw it, so the frame it is in decides what it shows.
void engine_draw_loading_screen(Engine* engine);

// Lift it: once its ident has played, the set switches off and the window shows the frame again.
void engine_hide_loading_screen(Engine* engine);

// True from show until the switch-off has finished.
bool engine_loading_screen_shown(const Engine* engine);

#endif
