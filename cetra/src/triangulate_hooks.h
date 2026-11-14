#ifndef _TRIANGULATE_HOOKS_H_
#define _TRIANGULATE_HOOKS_H_

/*
 * What a test reads of triangulate.c (spec 14.37): the triangulation as earcut makes it, every
 * hole's bridge found by walking the ring, for the grid's bridges to be held against.
 */

#include "triangulate.h"

// triangulate_polygon with every bridge found by the walk, however many holes.
bool triangulate_polygon_walked(const double* xy, size_t count, const size_t* holes,
                                size_t hole_count, Triangulation* out);

#endif
