#ifndef _TRIANGULATE_HOOKS_H_
#define _TRIANGULATE_HOOKS_H_

/*
 * What a test reads of triangulate.c (spec 14.37): the triangulation with the bridges found the
 * way the test chooses, so the grid's can be held to the walk's on any polygon.
 */

#include "triangulate.h"

// triangulate_polygon, each hole's bridge found through the grid from `grid_holes` holes and by
// walking the ring below: 0 for the grid always, SIZE_MAX for the walk always.
bool triangulate_polygon_bridged(const double* xy, size_t count, const size_t* holes,
                                 size_t hole_count, size_t grid_holes, Triangulation* out);

#endif // _TRIANGULATE_HOOKS_H_
