#ifndef _SILENT_TOWER_H_
#define _SILENT_TOWER_H_

#include <cglm/cglm.h>

#include "kit.h"

// The octagonal tower on the house's front corner (spec 13.13): its walls and windows, the
// parlour's bay below and the study's above, the study's high ceiling, and the spire.
void tower_build(Kit* kit);

/*
 * The rectangle x0..x1, z0..z1 with the tower taken out of its corner at (x0, z0), which must
 * lie inside the octagon of `apothem` while the rest of the outline crosses the rectangle's
 * two edges from that corner: the outline of a floor, a ceiling or a roof the tower stands
 * through. Returns its corner count, at most 5 + 8, into `out`.
 */
int tower_notch(float x0, float z0, float x1, float z1, float apothem, vec2* out);

// How far `p` is outside the tower's outer faces in plan; negative inside.
float tower_outside_distance(const vec3 p);

// How far `p` is in plan from the nearer face of the nearest of the tower's eight walls, taken
// whole: negative inside one.
float tower_wall_distance(const vec3 p);

#endif // _SILENT_TOWER_H_
