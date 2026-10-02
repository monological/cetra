#ifndef _SILENT_STUDY_H_
#define _SILENT_STUDY_H_

#include "cetra/scene.h"

#include "kit.h"

/*
 * The study (spec 13.13), over the parlour and up into the tower's bay: Gothic bookcases round
 * its three plain walls, filled with books whose spines are cut from gothic.h's strips, seeded;
 * a library ladder on a brass rail; and in the bay under the stained glass a carved desk with
 * its chair, a green-shaded lamp, unlit candles, an open journal, letters, an inkwell, and a
 * globe on its stand. The lamp's light goes into the scene, so this comes after the flashlight.
 */
void study_build(Kit* kit, Scene* scene, unsigned int seed);

#endif // _SILENT_STUDY_H_
