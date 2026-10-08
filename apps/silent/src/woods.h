#ifndef _SILENT_WOODS_H_
#define _SILENT_WOODS_H_

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "fences.h"
#include "kit.h"
#include "trees.h"

/*
 * The woods behind both sides of the street (spec 13.35): conifers from the engine's generator,
 * with the dead trees mixed in, stumps, fallen logs and boulders on a floor of earth and twigs,
 * thinner along the fences and closing up behind. A dead tree lies across each fence that has come
 * down. Everything is hung under the scene once, here.
 */
void woods_build(Kit* kit, Engine* engine, Scene* scene, Trees* trees,
                 const FenceBreaches* breaches, unsigned int seed);

#endif // _SILENT_WOODS_H_
