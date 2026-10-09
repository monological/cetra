#ifndef _SILENT_TOWN_PLAN_H_
#define _SILENT_TOWN_PLAN_H_

#include <stdbool.h>

#include "street.h"

/*
 * The town's plan as text (spec 13.43), which tools/make_map.py draws the town's map from: every
 * shape the printed map shows, read from the code that builds it, so nothing is copied into the
 * tool by hand. One record a line, in a fixed order, metres to three places, x east and z south.
 */

// The plan of the town built from `plots` with `seed`, to `path`; false, with the reason printed,
// when it cannot be written.
bool town_plan_write(const char* path, const StreetPlots* plots, unsigned int seed);

#endif // _SILENT_TOWN_PLAN_H_
