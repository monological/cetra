#ifndef _SILENT_CROSSROADS_H_
#define _SILENT_CROSSROADS_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "kit.h"
#include "street.h"

/*
 * The street's west end (spec 13.35): a crossroads, the cross street's arms each closed by a
 * barricade -- sawhorses with a ROAD CLOSED sign, chain-link panels on feet, and on the north arm
 * a police car left across the road -- and straight on, the road broken off at the lip of a chasm
 * the fog fills: slabs gone over the edge, rebar, a drain pipe cut through, a guard rail torn, a
 * utility pole leaning out with its wires hanging into the hole, and over it all a lamp, failing.
 */
void crossroads_build(Kit* kit, Scene* scene, bool night, FailingLamp* failing);

#endif // _SILENT_CROSSROADS_H_
