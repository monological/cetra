#ifndef _SILENT_BEDROOM_H_
#define _SILENT_BEDROOM_H_

#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

#include "kit.h"

/*
 * The player's bedroom (spec 13.40), the back room east of the hall: a double bed with its head
 * to the east window, a nightstand either side of it with a lamp on one, and a chest of drawers
 * on the hall wall facing the bed, in the stretch the door leaves clear when it stands open.
 * Where the room is, is home.h's.
 */

// Where something lying on the bed rests: the middle of the quilt's top near its foot.
void bedroom_bed_top(vec3 out);

// The bed, the nightstands, the dresser, the rug and what stands on them into `kit`, and the lamp,
// a kit of its own, with its light.
void bedroom_build(Kit* kit, Engine* engine, Scene* scene);

#endif // _SILENT_BEDROOM_H_
