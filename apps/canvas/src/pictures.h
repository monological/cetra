#ifndef CANVAS_PICTURES_H
#define CANVAS_PICTURES_H

#include <stdbool.h>

#include "cetra/canvas.h"
#include "cetra/engine.h"

// The pictures spec 14.41 reads back, each written as a PPM every frame it is read, so a headless
// run's last is its final frame's, beside the run's screenshot:
//   --read-window X,Y,W,H,PATH  a rectangle of the window, in points
//   --read-canvas PPP,PATH      the first canvas view off the screen, PPP 0 the window's ratio
//   --read-view3d PATH          the 3D pane's own picture (with --view3d)
//   --offscreen-view PATH       a second view of the 3D pane's scene, offscreen, at its size and
//                               camera, read whenever it drew (with --view3d)
// Each says, once, where its picture stands in the window's framebuffer pixels, for a gate to cut
// the same rectangle out of the screenshot. And the window that cannot draw:
//   --minimize N                the window minimizes itself after its second frame, asks a timed
//                               wake every update and a window read once, says each update it
//                               gets and anything drawn meanwhile, restores itself after N, and
//                               ends the run once it has drawn again

// Takes argv[*i] and its value when it is one of the four; false when it is not.
bool pictures_parse(int argc, char** argv, int* i);
// The offscreen view made, beside the 3D pane `view3d`, when one was asked for. False, said, when
// it could not be.
bool pictures_start(Engine* engine, const SceneView* view3d);
// The offscreen view held to the 3D pane's size, camera and settings: from the pre-render hook,
// after the panes are laid out.
void pictures_lay_out(const SceneView* view3d);
// The reads asked, from the overlay that draws `view`, the first canvas view.
void pictures_read(Engine* engine, Canvas* canvas, const CanvasView* view, const SceneView* view3d);
// --minimize's turn, from the update hook.
void pictures_update(Engine* engine);

#endif
