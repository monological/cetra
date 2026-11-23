#ifndef _PINCH_H_
#define _PINCH_H_

/*
 * The trackpad's pinch on a window (spec 14.44). GLFW hands an app none on any system, so cetra
 * hears it from the system itself where it can: macOS, in pinch_macos.m. Everywhere else there is
 * no watch to make, and the engine's pinch stays 0.
 */

struct GLFWwindow;
typedef struct PinchWatch PinchWatch;

// One change of a pinch: the part of itself what is pinched grows by, + as the fingers part.
typedef void (*PinchFn)(void* user, double by);

// A watch on `window`'s pinches, each handed to `heard` on the thread that polls the window's
// events, inside the poll. NULL where the system gives none to hear, and, logged, where one
// cannot be made.
PinchWatch* create_pinch_watch(struct GLFWwindow* window, PinchFn heard, void* user);
void free_pinch_watch(PinchWatch* watch);

#endif // _PINCH_H_
