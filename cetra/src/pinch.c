#include "pinch.h"

#include <stddef.h>

// macOS's watch is pinch_macos.m's. No other system's pinch is heard yet (spec 14.44, Not in
// this spec).
#ifndef __APPLE__

PinchWatch* create_pinch_watch(struct GLFWwindow* window, PinchFn heard, void* user) {
    (void)window;
    (void)heard;
    (void)user;
    return NULL;
}

void free_pinch_watch(PinchWatch* watch) {
    (void)watch;
}

#endif
