#ifndef _SILENT_TREES_H_
#define _SILENT_TREES_H_

#include "cetra/scene.h"

#include "kit.h"

struct Engine;

// The dead trees along the drive and round the mansion's grounds (spec 13.25): bare, twisted,
// swaying in the scene's wind, each standing on the hill with a collider round its trunk.
void trees_build(Kit* kit, struct Engine* engine, Scene* scene, unsigned int seed);

#endif // _SILENT_TREES_H_
