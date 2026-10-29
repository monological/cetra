#ifndef _TRIANGULATE_H_
#define _TRIANGULATE_H_

/*
 * Polygon triangulation (spec 14.0): an outer outline and the holes in it, as
 * triangles, by ear clipping with holes bridged in -- Mapbox's earcut, ported.
 * Pure CPU and no global state, so any thread may call it.
 *
 * Each outline is a ring of points in either winding. A self-intersecting
 * outline is triangulated as well as earcut manages, which may leave part of
 * it out; a simple one is covered exactly. Every triangle's corners are points
 * given: none is added.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Triangulation {
    uint32_t* indices; // three a triangle, into the points given
    size_t count;      // triangles
} Triangulation;

// `xy` holds `count` points, x then y; the outer outline is the points up to
// the first hole's start, and each hole runs from its start to the next
// hole's or the end. False, with `out` empty, for fewer than three points, a
// hole of fewer than three, or out of memory.
bool triangulate_polygon(const double* xy, size_t count, const size_t* holes, size_t hole_count,
                         Triangulation* out);
void free_triangulation(Triangulation* triangulation);

#endif
