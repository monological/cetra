#ifndef CANVAS_VIEW3D_H
#define CANVAS_VIEW3D_H

#include "cetra/engine.h"
#include "cetra/scene.h"

// --view3d (spec 14.38): a small lit scene built in code into `scene` -- three spheres, a metal, a
// plastic and a rough one, on a floor under the corpus's window environment, with one key light
// from its brightest lobe casting shadows -- and a view of it, added to `engine`, whose rectangle
// the caller lays out. NULL, said, when the view cannot be made.
SceneView* view3d_build(Engine* engine, Scene* scene);

#endif
