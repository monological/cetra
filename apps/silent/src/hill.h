#ifndef _SILENT_HILL_H_
#define _SILENT_HILL_H_

#include <stdbool.h>

#include "kit.h"

// The drive that climbs past the street's east end to the mansion's grounds at MANSION_Y, and the
// grounds' box (spec 13.25).
void hill_build(Kit* kit);

// The hill's height at (x, z): 0 short of the street's east end, the mansion's grounds at its top,
// the drive carved in.
float hill_height(float x, float z);

// Whether (x, z) is on the mansion's grounds, which are flat and stand on a box.
bool hill_on_grounds(float x, float z);

// The hill's lumps at (x, z), about -1.5..1.5: two octaves of value noise, the same at any call.
float hill_lumps(float x, float z);

// How far (x, z) is from the drive's centre line, in plan.
float hill_drive_distance(float x, float z);

// The drive's centre line at `t` of its length, 0 at the street's end and 1 at the gate, and the
// way it runs there, a unit vector in plan.
void hill_drive_point(float t, float* x, float* z);
void hill_drive_frame(float t, float* x, float* z, float* dir_x, float* dir_z);

#endif // _SILENT_HILL_H_
