#ifndef _SILENT_HOUSE_H_
#define _SILENT_HOUSE_H_

#include "houses.h"
#include "kit.h"

// The player's house: floors, ceilings, walls with their openings, the porch, and the
// gutter along the front eave with its downpipe, whose leaks go on `drips`. What stands
// in the rooms is kitchen.c's.
void house_build(Kit* kit, Drips* drips);

#endif // _SILENT_HOUSE_H_
