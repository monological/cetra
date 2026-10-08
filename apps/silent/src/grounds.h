#ifndef _SILENT_GROUNDS_H_
#define _SILENT_GROUNDS_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "kit.h"
#include "street.h"

// What stands about the drive and the mansion's grounds (spec 13.25): iron gates on stone piers
// at the grounds' front, one leaf hanging open; a rusted iron fence round the grounds, gapped; a
// path from the gate to the porch; a small graveyard beside the drive; and two of the street's
// lamps up the drive, one dead and one failing.
void grounds_build(Kit* kit, Scene* scene, unsigned int seed, bool night, FailingLamp* failing);

#endif // _SILENT_GROUNDS_H_
