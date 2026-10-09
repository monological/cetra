#ifndef _SILENT_LAND_H_
#define _SILENT_LAND_H_

#include "kit.h"

// The height of the far side's `lot` (spec 13.35), 0 the westmost: the terrace's lots are level,
// each a step higher than the one east of it, from TERRACE_LOW at the street's east end to
// TERRACE_HIGH at its west.
float land_terrace_lot_height(int lot);
// The height of the lot x stands on, between the lines far_lot_line_x places; past either end,
// that end's lot.
float land_terrace_height(float x);

// The ground's height at (x, z) anywhere in the world: the street's level on its plate and our
// yards, the lot's on the terrace, the crossroads' level round them, and elsewhere the woods
// climbing behind the far side, falling behind ours, and the hill past the east end. What to
// stand anything on.
float land_height(float x, float z);

// Whether the ground's facet at (x, z) is too steep to hold soil: where it shows the rock.
bool land_too_steep(float x, float z);

// Where the ground ends at the chasm along the line z: west of it there is none, as far south as
// RIDGE_Z, where the lip turns west.
float land_lip_x(float z);
// And from there on west, where it ends along the line x: north of it there is none.
float land_ridge_lip_z(float x);
// How far (x, z) stands back from the chasm's lip, either one, beyond `clear`: negative nearer.
// FLT_MAX past the corner on the valley's side, where there is no lip.
float land_lip_clearance(float x, float z, float clear);

// The ground grid's cell, along both axes; its lines run from the lip and from WORLD_Z0.
#define LAND_STEP 2.0f

// The ground outside the street's plate and the terrace's lots, which are flat and stand on boxes:
// faceted over a grid from land_height, and collided on the same grid; and the chasm's face down
// from the lip, with a body along its top.
void land_build(Kit* kit);

#endif // _SILENT_LAND_H_
