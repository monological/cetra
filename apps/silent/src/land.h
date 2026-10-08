#ifndef _SILENT_LAND_H_
#define _SILENT_LAND_H_

#include "kit.h"

// The far side's lot at x (spec 13.35): six level lots, each a step higher than the one east of
// it, from TERRACE_LOW at the street's east end to TERRACE_HIGH at its west. Past either end, that
// end's lot.
float land_terrace_height(float x);
// Which of the six lots x stands on, 0 the westmost, and where that lot begins and ends along x.
int land_terrace_lot(float x);
void land_terrace_lot_span(int lot, float* x0, float* x1);

// The ground's height at (x, z) anywhere in the world: the street's level on its plate and our
// yards, the lot's on the terrace, and elsewhere the woods climbing behind the far side, falling
// behind ours, and the hill past the east end. What to stand anything on.
float land_height(float x, float z);

// The ground outside the street's plate and the terrace's lots, which are flat and stand on boxes:
// faceted over a grid from land_height, and collided on the same grid. After hill_build, whose
// drive the hill's term carves.
void land_build(Kit* kit);

#endif // _SILENT_LAND_H_
