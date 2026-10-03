#ifndef _SILENT_CAT_DEBUG_H_
#define _SILENT_CAT_DEBUG_H_

#include "cetra/engine.h"

#include "cat.h"
#include "cat_brain.h"

/*
 * Seeing the cat think (spec 13.17), while the GUI is up: the house's graph drawn over the
 * frame -- every link by kind, the route ahead, the places by name, its line of sight and what
 * it watches -- and a panel with its scores and needs, a place to send it to, and its colours.
 * The mind's half is left out when it has none, as under --cat-goto.
 */

void cat_debug_draw(Cat* cat, CatMind* mind, Engine* engine);

#endif // _SILENT_CAT_DEBUG_H_
