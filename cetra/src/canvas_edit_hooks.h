#ifndef _CANVAS_EDIT_HOOKS_H_
#define _CANVAS_EDIT_HOOKS_H_

/*
 * What a test reads of an edit (spec 14.37): the pieces a polygon was cut into, its fill and its
 * outline's edge records, so the mesh can be checked whole, with no pixel standing in for it.
 */

#include "canvas.h"

// The fill pieces the edit holds, every polygon's together, in the order added: up to `cap`
// pieces' corners into `corners`, three a piece, in units. Returns how many it holds, so a call
// with no room asks the count.
size_t canvas_edit_fill_corners(const CanvasEdit* edit, CanvasPoint* corners, size_t cap);
// The edge records likewise, each a piece of an outline edge with its polygon left of a to b: up
// to `cap` records' ends into `ends`, a then b, in units.
size_t canvas_edit_edge_ends(const CanvasEdit* edit, CanvasPoint* ends, size_t cap);
// The fill pieces the edit dropped as their corners rounded onto one line.
size_t canvas_edit_flat_fills(const CanvasEdit* edit);

#endif // _CANVAS_EDIT_HOOKS_H_
