#include <math.h>

#include "hill.h"
#include "lake.h"
#include "layout.h"
#include "mats.h"
#include "road.h"
#include "street.h"

/*
 * The lake valley's ground (spec 13.41). The world's own ground runs on south past the woods
 * behind our side and west past the chasm's turn; this carves into it, last, what the valley
 * needs:
 *
 *   - a rock ridge along the chasm's south lip west of where it turns, so the chasm ends at
 *     ground that stands over it rather than at a level edge;
 *   - a bowl round the water, falling to the waterline over a few tens of metres, furthest on
 *     the side the track and the cabin are, so the bank there is gentle;
 *   - under the water a shelf to wading depth, then a drop to a lumpy floor;
 *   - the track down from the cross street, and the shoulder up to the cabin's pad.
 *
 * Each term is exactly nothing past its reach, so the ground everywhere else is the world's to
 * the bit.
 */

#define RIDGE_RISE    4.0f     // metres the ridge stands over the ground round it
#define RIDGE_CREST_Z 80.0f    // along this line
#define RIDGE_HALF    14.0f    // metres either side of it, over which it falls back
#define RIDGE_X0      (-74.0f) // it starts west of the chasm's corner and is whole by RIDGE_X1
#define RIDGE_X1      (-86.0f)

#define BOWL_REACH  28.0f    // metres from the shore over which the bank climbs back to the ground
#define BOWL_LONG   24.0f    // and this much further on the side toward the track and the cabin
#define BOWL_FACING (-0.54f) // radians: that side's bearing from the lake's centre
#define BEACH_SLOPE 0.06f    // the bank's rise per metre at the water's edge

#define SHELF_SLOPE 0.12f // the bed's fall per metre out from the shore: 0.6 m at the wading ring
#define DROP        3.0f  // and then down, between DROP_FROM and DROP_TO metres out
#define DROP_FROM   LAKE_WADE_OUT
#define DROP_TO     14.0f
#define FLOOR_FALL  0.6f // a last fall to the deepest water, out to FLOOR_TO
#define FLOOR_TO    30.0f
#define FLOOR_LUMPS 0.25f // metres of lumps on the floor

#define PAD_SHOULDER 3.0f // metres over which the ground comes to the pad's level

// The track's line: on down the cross street's south arm, through the old cutting, round the
// ridge's east end, and down the bank to the cabin's pad.
static const float TRACK_POINTS[][2] = {
    {-52.0f, 26.0f}, {-52.0f, 36.0f}, {-51.0f, 48.0f}, {-50.0f, 60.0f},  {-54.0f, 72.0f},
    {-63.0f, 80.0f}, {-73.0f, 88.0f}, {-77.0f, 98.0f}, {-71.0f, 106.0f}, {-65.0f, 113.0f},
};

// Level over the asphalt it starts on and at the pad, between them one grade, under the 8 degrees
// puddles stand on, eased in and out over ten metres.
static const RoadDesc TRACK = {
    .points = TRACK_POINTS,
    .point_count = (int)(sizeof(TRACK_POINTS) / sizeof(TRACK_POINTS[0])),
    .half = 1.8f,
    .shoulder = 4.0f,
    .y0 = ROAD_Y,
    .y1 = CABIN_PAD_Y,
    .flat_run = 10.0f,
    .flat_end = 3.0f,
    .rise = ROAD_RISE_GRADE,
    .ease = 10.0f,
};

static Road g_track = {.desc = &TRACK};

// The shore's radius from the lake's centre at bearing `theta`: an ellipse, wobbled a little
// everywhere but due east, where the cabin's bank is.
static float shore_radius(float theta) {
    const float c = LAKE_RZ * cosf(theta), s = LAKE_RX * sinf(theta);
    const float ellipse = LAKE_RX * LAKE_RZ / sqrtf(c * c + s * s);
    return ellipse * (1.0f + 0.07f * sinf(2.0f * theta) + 0.05f * sinf(3.0f * theta));
}

float lake_shore_distance(float x, float z) {
    const float dx = x - LAKE_X, dz = z - LAKE_Z;
    return hypotf(dx, dz) - shore_radius(atan2f(dz, dx));
}

float lake_track_distance(float x, float z) {
    float along = 0.0f;
    return road_distance(&g_track, x, z, &along);
}

// The valley's own shape, before the track and the pad: the ridge, the bowl and the bed.
static float valley(float x, float z, float h) {
    const float ridge = RIDGE_RISE *
                        (1.0f - glm_smoothstep(0.0f, RIDGE_HALF, fabsf(z - RIDGE_CREST_Z))) *
                        glm_smoothstep(RIDGE_X0, RIDGE_X1, x);
    if (ridge > 0.0f)
        h += ridge;
    const float dx = x - LAKE_X, dz = z - LAKE_Z;
    const float theta = atan2f(dz, dx);
    const float d = hypotf(dx, dz) - shore_radius(theta);
    if (d >= 0.0f) {
        const float reach = BOWL_REACH + BOWL_LONG * fmaxf(0.0f, cosf(theta - BOWL_FACING));
        if (d >= reach)
            return h;
        const float s = glm_smoothstep(0.0f, reach, d);
        return LAKE_Y + (h - LAKE_Y) * s + BEACH_SLOPE * d * (1.0f - s);
    }
    const float u = -d;
    const float depth = SHELF_SLOPE * u + DROP * glm_smoothstep(DROP_FROM, DROP_TO, u) +
                        FLOOR_FALL * glm_smoothstep(DROP_TO, FLOOR_TO, u) +
                        FLOOR_LUMPS * hill_lumps(x, z) * glm_smoothstep(DROP_TO, DROP_TO + 4.0f, u);
    return LAKE_Y - depth;
}

float lake_ground(float x, float z, float h) {
    h = road_carve(&g_track, x, z, valley(x, z, h));
    const float pad =
        plan_box_distance(x, z, CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1);
    if (pad >= PAD_SHOULDER)
        return h;
    return h + (CABIN_PAD_Y - h) * (1.0f - glm_smoothstep(0.0f, PAD_SHOULDER, pad));
}

// Whether the track's centre `s` metres along is on the cabin's pad.
static bool on_pad(float s) {
    float x = 0.0f, z = 0.0f;
    road_at(&g_track, s, &x, &z);
    return plan_box_distance(x, z, CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1) <= 0.0f;
}

void lake_ground_build(Kit* kit) {
    // The surface from exactly where the cross street's asphalt ends -- a point of the track's
    // own line -- to exactly where the pad begins, so it is laid over neither.
    float s0 = 0.0f;
    road_distance(&g_track, CROSS_X, CROSS_Z1, &s0);
    float lo = s0, hi = road_length(&g_track);
    for (int k = 0; k < 24; k++) {
        const float mid = 0.5f * (lo + hi);
        if (on_pad(mid))
            hi = mid;
        else
            lo = mid;
    }
    road_ribbon(kit, &g_track, MAT_TRACK, s0, lo);
    // The pad: packed gravel, level, on a box like the street's flat ground.
    street_ground(kit, MAT_TRACK, CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1,
                  CABIN_PAD_Y);
}
