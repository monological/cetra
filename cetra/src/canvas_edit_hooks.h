#ifndef _CANVAS_EDIT_HOOKS_H_
#define _CANVAS_EDIT_HOOKS_H_

/*
 * What a test reads of an edit (spec 14.37): the fill pieces it holds, so the mesh a polygon was
 * cut into can be checked whole, with no pixel standing in for it.
 */

#include "canvas.h"

// The fill pieces the edit holds, every polygon's together, in the order added.
size_t canvas_edit_fill_count(const CanvasEdit* edit);
// Fill piece i's three corners, in units, in its own order.
void canvas_edit_fill_corners(const CanvasEdit* edit, size_t i, CanvasPoint corners[3]);

#endif
