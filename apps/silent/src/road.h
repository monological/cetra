#ifndef _SILENT_ROAD_H_
#define _SILENT_ROAD_H_

#include "kit.h"

/*
 * A road carved into the ground (spec 13.41): a line through points by Catmull-Rom, sampled at
 * even steps of its length, a height along it, and the carve that brings the ground to that
 * height under it and back up to its own over a shoulder. The mansion's drive is one; the track
 * down to the lake is another.
 */

#define ROAD_SAMPLES 512 // at ROAD_STEP, so no road is longer than 255.5 m
#define ROAD_STEP    0.5f

typedef enum RoadRise {
    ROAD_RISE_SMOOTH, // a smoothstep from y0 to y1 between the flat ends: steepest at its middle
    ROAD_RISE_GRADE,  // one grade between the flat ends, eased in and out over `ease`
} RoadRise;

typedef struct RoadDesc {
    const float (*points)[2]; // its line in plan, start to end
    int point_count;
    float half;     // the surface's half-width
    float shoulder; // over which the ground comes back up to its own
    float y0, y1;   // the height at the start and at the end
    float flat_run; // metres level at the start before it rises or falls
    float flat_end; // and at the end
    RoadRise rise;
    float ease; // ROAD_RISE_GRADE: metres the grade eases in and out over
} RoadDesc;

typedef struct Road {
    const RoadDesc* desc;
    // Sampled the first time anything asks, so the road answers whenever it is asked.
    float x[ROAD_SAMPLES], z[ROAD_SAMPLES], s[ROAD_SAMPLES];
    int count;
    float length;
    float box[4]; // the samples' bounds in plan: min x, min z, max x, max z
} Road;

// The height `s` metres along the road.
float road_height(Road* road, float s);

// How far (x, z) is from the road's centre line, in plan, and how far along it the nearest
// point is, unless `along` is NULL.
float road_distance(Road* road, float x, float z, float* along);

// The ground `h` at (x, z) with the road carved in: a hair under the surface across its width,
// back to `h` over the shoulder, and `h` exactly beyond.
float road_carve(Road* road, float x, float z, float h);

// The centre line at `t` of the length, 0 at the start and 1 at the end, and the way it runs
// there, a unit vector in plan. At the nearest sample.
void road_point(Road* road, float t, float* x, float* z);
void road_frame(Road* road, float t, float* x, float* z, float* dir_x, float* dir_z);

// The road's length, and its centre line `s` metres along it, between the samples.
float road_length(Road* road);
void road_at(Road* road, float s, float* x, float* z);

// The surface, level across, as a ribbon of `mat` from exactly `s0` to exactly `s1` metres along.
void road_ribbon(Kit* kit, Road* road, int mat, float s0, float s1);

#endif // _SILENT_ROAD_H_
