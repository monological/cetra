#ifndef CANVAS_MINIMIZE_H
#define CANVAS_MINIMIZE_H

#include <stdbool.h>

#include "cetra/engine.h"

// --minimize N (spec 14.41): the window minimizes itself after its second frame, asks a timed wake
// of 0.05 s every update and one window read, says each update it gets while minimized with the
// time since the last and anything drawn meanwhile, restores itself after N, and ends the run once
// it has drawn again. In a window: headless, a minimized hidden window still plays the Dock's
// animation.

// Takes argv[*i] and its value when it is --minimize; false when it is not.
bool minimize_parse(int argc, char** argv, int* i);
// Its turn, from the update hook.
void minimize_update(Engine* engine);
// A frame drawn, from the overlay: counted while minimized, and the run ended once restored.
void minimize_drawn(Engine* engine);

#endif
