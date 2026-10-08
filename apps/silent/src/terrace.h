#ifndef _SILENT_TERRACE_H_
#define _SILENT_TERRACE_H_

#include "houses.h"
#include "kit.h"
#include "layout.h"

// The far side's terrace (spec 13.35): its six lots, the retaining wall along the back of the far
// sidewalk with its returns at both ends and a rail along its top, and a flight of stairs up
// through the wall to each lot's front door, `far` a lot each from the west. After land_build,
// since the east return runs up into the woods' slope.
void terrace_build(Kit* kit, const HousePlot far[TERRACE_LOTS]);

#endif // _SILENT_TERRACE_H_
