#ifndef _SILENT_HILL_H_
#define _SILENT_HILL_H_

#include "kit.h"

// The ground past the street's east end, up to the mansion's grounds at MANSION_Y, and the drive
// that climbs it (spec 13.25).
void hill_build(Kit* kit);

// The ground's height at (x, z) past the street's east end: what anything standing on the hill
// stands on.
float hill_height(float x, float z);

// How far (x, z) is from the drive's centre line, in plan.
float hill_drive_distance(float x, float z);

// The drive's centre line at `t` of its length, 0 at the street's end and 1 at the gate, and the
// way it runs there, a unit vector in plan.
void hill_drive_point(float t, float* x, float* z);
void hill_drive_frame(float t, float* x, float* z, float* dir_x, float* dir_z);

#endif // _SILENT_HILL_H_
