#ifndef _SILENT_GROUNDS_H_
#define _SILENT_GROUNDS_H_

#include <stdbool.h>

#include "cetra/light.h"
#include "cetra/scene.h"

#include "kit.h"

// What stands about the drive and the mansion's grounds (spec 13.25): iron gates on stone piers
// at the grounds' front, one leaf hanging open; a rusted iron fence round the grounds, gapped; a
// path from the gate to the porch; a small graveyard beside the drive; and two of the street's
// lamps up the drive, one dead and one failing.
typedef struct Grounds {
    Light* failing; // the lamp that flickers, NULL by day
    float base_intensity;
} Grounds;

void grounds_build(Grounds* grounds, Kit* kit, Scene* scene, unsigned int seed, bool night);

// The failing lamp's flicker at `time` seconds.
void grounds_update(Grounds* grounds, double time);

#endif // _SILENT_GROUNDS_H_
