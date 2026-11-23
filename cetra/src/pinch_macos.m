// The trackpad's pinch on macOS (spec 14.44): AppKit's magnify event, which GLFW's view has no
// handler for and so lets pass. A local monitor sees every event of the kind before the app
// hands it on, and hands it on untouched.

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <stdlib.h>

#include "ext/log.h"
#include "pinch.h"

struct PinchWatch {
    id monitor; // AppKit's, held until it is removed
};

PinchWatch* create_pinch_watch(GLFWwindow* window, PinchFn heard, void* user) {
    NSWindow* const watched = window ? glfwGetCocoaWindow(window) : nil;
    if (!watched || !heard) {
        log_error("create_pinch_watch: no window of AppKit's to watch, or nothing to tell");
        return NULL;
    }
    PinchWatch* watch = calloc(1, sizeof(*watch));
    if (!watch) {
        log_error("create_pinch_watch: out of memory");
        return NULL;
    }
    // `magnification` is the change in scale since the gesture's last event, the unit the engine
    // sums. The window is compared and never sent a message: the watch is freed before it is.
    watch->monitor = [[NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                     handler:^NSEvent*(NSEvent* event) {
                                       if ([event window] == watched)
                                           heard(user, (double)[event magnification]);
                                       return event;
                                     }] retain];
    return watch;
}

void free_pinch_watch(PinchWatch* watch) {
    if (!watch)
        return;
    [NSEvent removeMonitor:watch->monitor];
    [watch->monitor release];
    free(watch);
}
