#include <float.h>
#include <math.h>
#include <stdio.h>

#include "cetra/ext/log.h"
#include "cetra/game/audio.h"
#include "cetra/scene.h"
#include "cetra/water.h"

#include "hill.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "road.h"
#include "sounds.h"
#include "street.h"
#include "trees.h"

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

/*
 * The lake itself (spec 13.41).
 *
 * The engine's water is a plane to the horizon unless it is told where it ends, so it is held
 * to a rectangle round the basin whose every edge is under the bank: outside it there is no
 * water, and nothing below LAKE_Y elsewhere -- the chasm -- floods or darkens. It is drawn only
 * while the eye is in or near the valley, which the fog hides; in clear air, everywhere.
 *
 * Dark and still: a peat lake in the woods, tea-brown and opaque at arm's length, under a breath
 * of wind. No caustics and no motes, which such water would not show.
 */

#define WATER_X0 (-124.0f) // where the water is: round the basin, its edges under the bank
#define WATER_X1 (-68.0f)
#define WATER_Z0 97.0f
#define WATER_Z1 141.0f
// The eye is in the valley past the first of these and out of it before the second, so the
// water does not flick on and off with a step back and forth.
#define WATER_ON_X  (-40.0f)
#define WATER_ON_Z  66.0f
#define WATER_OFF_X (-36.0f)
#define WATER_OFF_Z 60.0f

#define RING_FOOT \
    (-12.9f) // the wading ring's bodies, from under the bed at the ring to over a head
#define RING_TOP  (-10.4f)
#define WAKE_PACE 0.6f // metres a wader moves for each ripple

#define DOCK_X0   (-72.0f)         // where its deck leaves the bank
#define DOCK_X1   (-85.0f)         // and its far end, over deep water
#define DOCK_Z    118.0f           // its centre line, straight out from the cabin's door
#define DOCK_HALF 0.8f             // half its width
#define DOCK_Y    (-11.05f)        // the deck's top, a metre over the water
#define DOCK_POST (DOCK_X1 + 0.3f) // the last pair of posts, standing up past the deck

#define BOAT_HALF_LEN  1.8f
#define BOAT_HALF_BEAM 0.62f
#define BOAT_DEPTH     0.42f  // keel below the gunwale amidships
#define BOAT_SKIN      0.035f // the planking's thickness
#define BOAT_STATIONS  14
#define BOAT_GIRTH     5

#define REED_BEDS     18 // clumps of clumps
#define DROWNED_TREES 4

// Against the other loops tools/fetch_sounds.py levels alike.
#define LAPPING_VOLUME 0.35f

static WaterWake g_wake;
static bool g_always_drawn;
static Sound* g_lapping;

// The point `out` metres outside the shore (negative: over the water) on bearing `theta`.
static void shore_at(float theta, float out, float* x, float* z) {
    const float r = shore_radius(theta) + out;
    *x = LAKE_X + r * cosf(theta);
    *z = LAKE_Z + r * sinf(theta);
}

static void water(Scene* scene, bool always_drawn) {
    Water* w = create_water();
    if (!w)
        return;
    w->level = LAKE_Y;
    glm_vec4_copy((vec4){WATER_X0, WATER_Z0, WATER_X1, WATER_Z1}, w->bounds);
    // No bed: the surface reads its column off the scene's depth, which the basin is, and a bed
    // would only bring a shore's surf to a lake with none.
    w->height_at = NULL;
    w->amplitude = 0.015f;
    w->wavelength = 3.0f;
    w->steepness = 0.3f;
    glm_vec3_copy((vec3){1.0f, 1.4f, 2.2f}, w->absorption);
    glm_vec3_copy((vec3){0.002f, 0.003f, 0.003f}, w->scatter_albedo);
    w->caustics = false;
    w->specks = false;
    w->enabled = always_drawn;
    scene->water = w;

    // The bank must stand over the water all round the rectangle, or the plane shows past it.
    float lowest = FLT_MAX;
    for (float t = 0.0f; t <= 1.0f; t += 0.002f) {
        const float x = WATER_X0 + (WATER_X1 - WATER_X0) * t,
                    z = WATER_Z0 + (WATER_Z1 - WATER_Z0) * t;
        lowest = fminf(lowest, fminf(fminf(land_height(x, WATER_Z0), land_height(x, WATER_Z1)),
                                     fminf(land_height(WATER_X0, z), land_height(WATER_X1, z))));
    }
    printf("silent: lake: the lowest ground round the water's bounds is %.2f, %.2f over it\n",
           (double)lowest, (double)(lowest - LAKE_Y));
    if (lowest < LAKE_Y + 0.2f)
        log_warn("silent: the lake's bounds reach ground under its water: its edge will show");
}

// The ring's bearing where it crosses the line z on the east side, where the dock goes out.
static float ring_bearing(float z) {
    float lo = -0.8f, hi = 0.8f;
    for (int k = 0; k < 30; k++) {
        const float mid = 0.5f * (lo + hi);
        float x = 0.0f, zm = 0.0f;
        shore_at(mid, -LAKE_WADE_OUT, &x, &zm);
        if (zm < z)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5f * (lo + hi);
}

/*
 * The ring that stops a wader where the bed reaches wading depth: bodies on chords of that
 * contour, the long way round from one side of the dock to the other. The dock's own bodies
 * close the gap, so its deck stays walkable out over the deep water.
 */
static void ring(Kit* kit) {
    const float from = ring_bearing(DOCK_Z + DOCK_HALF + 0.3f);
    const float to = ring_bearing(DOCK_Z - DOCK_HALF - 0.3f) + 2.0f * GLM_PIf;
    const int chords = 48;
    float px = 0.0f, pz = 0.0f;
    shore_at(from, -LAKE_WADE_OUT, &px, &pz);
    for (int k = 1; k <= chords; k++) {
        const float t = from + (to - from) * (float)k / (float)chords;
        float x = 0.0f, z = 0.0f;
        shore_at(t, -LAKE_WADE_OUT, &x, &z);
        const float dx = x - px, dz = z - pz;
        kit_collider(kit, (vec3){0.5f * (x + px), 0.5f * (RING_TOP + RING_FOOT), 0.5f * (z + pz)},
                     (vec3){0.15f, 0.5f * (RING_TOP - RING_FOOT), 0.5f * hypotf(dx, dz) + 0.1f},
                     atan2f(dx, dz));
        px = x;
        pz = z;
    }
}

/*
 * The dock, straight out from the cabin's door: posts in pairs out of the bed, the last pair
 * standing up past the deck to tie a boat to, two stringers and planks across them, a few gone.
 * It leaves the bank where the bank has come down to it. One body to walk on, and past the
 * wading ring bodies down both its sides and across its end, so nobody steps off it into the
 * deep water.
 */
static void dock(Kit* kit, unsigned int* state) {
    const float z0 = DOCK_Z - DOCK_HALF, z1 = DOCK_Z + DOCK_HALF;
    const int pairs = 7;
    const float spacing = (DOCK_X0 - 0.4f - DOCK_POST) / (float)(pairs - 1);
    for (int i = 0; i < pairs; i++) {
        const float x = DOCK_X0 - 0.4f - spacing * (float)i;
        const float top = i == pairs - 1 ? DOCK_Y + 0.45f : DOCK_Y - 0.05f;
        for (int s = 0; s < 2; s++) {
            const float z = s ? z1 - 0.06f : z0 + 0.06f;
            kit_prism(kit, MAT_POLE, x, z, land_height(x, z) - 0.3f, top, 0.09f, 8, false);
        }
    }
    for (int s = 0; s < 2; s++)
        kit_frame_box(kit, &KIT_WORLD, MAT_FENCE_BOARD, DOCK_X0, DOCK_X1 + 0.2f, DOCK_Y - 0.21f,
                      DOCK_Y - 0.045f, s ? z1 - 0.18f : z0 + 0.08f, s ? z1 - 0.08f : z0 + 0.18f,
                      false);
    const float pitch = 0.185f;
    const int planks = (int)((DOCK_X0 - DOCK_X1) / pitch);
    for (int k = 0; k < planks; k++) {
        const float x = DOCK_X0 - pitch * ((float)k + 0.5f);
        const float gone = kit_xrnd(state), jitter = 0.04f * (kit_xrnd(state) - 0.5f);
        if (k > 12 && gone < 0.05f)
            continue;
        kit_frame_box(kit, &KIT_WORLD, MAT_PORCH, x - 0.08f, x + 0.08f, DOCK_Y - 0.045f, DOCK_Y,
                      z0 - 0.02f + jitter, z1 + 0.02f + jitter, false);
    }
    kit_frame_box(kit, &KIT_WORLD, KIT_COLLIDER_ONLY, DOCK_X0, DOCK_X1, DOCK_Y - 0.4f, DOCK_Y, z0,
                  z1, true);
    // From inside the ring out, down to below the bed and over a head.
    float ring_x = 0.0f, ring_z = 0.0f;
    shore_at(0.0f, -LAKE_WADE_OUT, &ring_x, &ring_z);
    const float bx0 = ring_x + 1.0f, bx1 = DOCK_X1 - 0.3f, foot = -16.5f, head = DOCK_Y + 1.0f;
    const float yc = 0.5f * (foot + head), yh = 0.5f * (head - foot);
    for (int s = 0; s < 2; s++)
        kit_collider(kit, (vec3){0.5f * (bx0 + bx1), yc, s ? z1 + 0.15f : z0 - 0.15f},
                     (vec3){0.5f * (bx0 - bx1), yh, 0.15f}, 0.0f);
    kit_collider(kit, (vec3){bx1 - 0.15f, yc, DOCK_Z}, (vec3){0.15f, yh, DOCK_HALF + 0.3f}, 0.0f);
}

/*
 * A rowboat, built round a frame at its gunwale's height amidships with a toward the bow: a
 * round-bilged clinker-less hull, its half-breadth fuller aft to a transom and running out to a
 * point at the bow, its sheer rising forward. Turned over, it lies keel up on its gunwales.
 */
typedef struct Boat {
    KitFrame f;
    bool upturned;
} Boat;

static float boat_breadth(float u) {
    return BOAT_HALF_BEAM * (u < 0.0f ? 1.0f - 0.35f * u * u : sqrtf(fmaxf(0.0f, 1.0f - u * u)));
}

// The boat's own (a, y, d) -- a toward the bow, y up from the gunwale, d to starboard -- into
// its frame, keel up when it is turned over.
static void boat_xyz(const Boat* b, float a, float y, float d, vec3 out) {
    out[0] = a;
    out[1] = b->upturned ? -y : y;
    out[2] = b->upturned ? -d : d;
}

// A point of the skin `inset` in from its outside, `u` along the boat (-1 the transom, 1 the
// bow), `phi` round its side from the gunwale (0) to the keel (pi / 2), on `side`.
static void boat_skin(const Boat* b, float u, float phi, float side, float inset, vec3 out) {
    const float breadth = fmaxf(boat_breadth(u) - inset, 0.0f);
    const float depth = BOAT_DEPTH * (1.0f - 0.25f * u * u) - inset;
    const float sheer = 0.06f * u * u + 0.12f * fmaxf(u, 0.0f) * u * u;
    boat_xyz(b, BOAT_HALF_LEN * u, sheer - depth * sinf(phi), side * breadth * cosf(phi), out);
}

static void boat_quad(Kit* kit, const Boat* b, int mat, vec3 p[4], float oa, float oy, float od) {
    vec3 out = {0.0f, 0.0f, 0.0f};
    boat_xyz(b, oa, oy, od, out);
    kit_frame_quad(kit, &b->f, mat, (const vec3*)p, out);
}

static void boat(Kit* kit, const Boat* b, bool oars) {
    // Painted outside, bare weathered boards within.
    for (int s = -1; s <= 1; s += 2)
        for (int i = 0; i < BOAT_STATIONS; i++)
            for (int j = 0; j < BOAT_GIRTH; j++) {
                const float u0 = -1.0f + 2.0f * (float)i / BOAT_STATIONS;
                const float u1 = -1.0f + 2.0f * (float)(i + 1) / BOAT_STATIONS;
                const float p0 = 0.5f * GLM_PIf * (float)j / BOAT_GIRTH;
                const float p1 = 0.5f * GLM_PIf * (float)(j + 1) / BOAT_GIRTH;
                const float pm = 0.5f * (p0 + p1);
                for (int inner = 0; inner < 2; inner++) {
                    const float inset = inner ? BOAT_SKIN : 0.0f, sign = inner ? -1.0f : 1.0f;
                    vec3 q[4];
                    boat_skin(b, u0, p0, (float)s, inset, q[0]);
                    boat_skin(b, u1, p0, (float)s, inset, q[1]);
                    boat_skin(b, u1, p1, (float)s, inset, q[2]);
                    boat_skin(b, u0, p1, (float)s, inset, q[3]);
                    boat_quad(kit, b, inner ? MAT_FENCE_BOARD : MAT_SIDING_B, q, 0.0f,
                              -sign * sinf(pm), sign * (float)s * cosf(pm));
                }
            }
    // The transom, both faces.
    for (int j = 0; j < BOAT_GIRTH; j++) {
        const float p0 = 0.5f * GLM_PIf * (float)j / BOAT_GIRTH;
        const float p1 = 0.5f * GLM_PIf * (float)(j + 1) / BOAT_GIRTH;
        for (int inner = 0; inner < 2; inner++) {
            const float inset = inner ? BOAT_SKIN : 0.0f;
            const float u = -1.0f + (inner ? BOAT_SKIN / BOAT_HALF_LEN : 0.0f);
            vec3 q[4];
            boat_skin(b, u, p0, 1.0f, inset, q[0]);
            boat_skin(b, u, p1, 1.0f, inset, q[1]);
            boat_skin(b, u, p1, -1.0f, inset, q[2]);
            boat_skin(b, u, p0, -1.0f, inset, q[3]);
            boat_quad(kit, b, inner ? MAT_FENCE_BOARD : MAT_SIDING_B, q, inner ? 1.0f : -1.0f, 0.0f,
                      0.0f);
        }
    }
    // A rail along each gunwale.
    for (int s = -1; s <= 1; s += 2) {
        vec3 rail[9];
        for (int k = 0; k < 9; k++)
            boat_skin(b, -1.0f + 2.0f * (float)k / 8.0f, 0.0f, (float)s, 0.5f * BOAT_SKIN, rail[k]);
        kit_frame_pipe(kit, &b->f, MAT_PORCH, rail, 9, 0.028f, 6);
    }
    // Two thwarts and the stern seat, across below the gunwale.
    static const float SEATS[3][2] = {{-0.82f, -0.62f}, {-0.2f, 0.0f}, {0.35f, 0.55f}};
    for (int k = 0; k < 3; k++) {
        const float u = 0.5f * (SEATS[k][0] + SEATS[k][1]);
        const float half = boat_breadth(u) * 0.93f - BOAT_SKIN;
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        boat_xyz(b, BOAT_HALF_LEN * SEATS[k][0], -0.2f, -half, lo);
        boat_xyz(b, BOAT_HALF_LEN * SEATS[k][1], -0.16f, half, hi);
        kit_frame_box(kit, &b->f, MAT_FENCE_BOARD, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], false);
    }
    if (!oars)
        return;
    // A pair of oars shipped along the thwarts, their blades aft.
    for (int s = -1; s <= 1; s += 2) {
        const float d = 0.18f * (float)s;
        vec3 shaft[2];
        boat_xyz(b, 1.25f, -0.13f, d, shaft[0]);
        boat_xyz(b, -1.05f, -0.13f, d * 1.3f, shaft[1]);
        kit_frame_pipe(kit, &b->f, MAT_PORCH, shaft, 2, 0.022f, 6);
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        boat_xyz(b, -1.05f, -0.135f, d * 1.3f - 0.07f, lo);
        boat_xyz(b, -1.6f, -0.125f, d * 1.3f + 0.07f, hi);
        kit_frame_box(kit, &b->f, MAT_PORCH, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], false);
    }
}

static void boats(Kit* kit) {
    // Moored off the dock's end on its south side, its bow to the tall post, floating a hand's
    // breadth deep, and tied to it.
    const Boat moored = {
        {{DOCK_X1 + 1.9f, LAKE_Y - 0.12f + BOAT_DEPTH, DOCK_Z - DOCK_HALF - 1.1f}, GLM_PIf + 0.06f},
        false};
    boat(kit, &moored, true);
    vec3 bow = {0.0f, 0.0f, 0.0f}, rope[3];
    kit_frame_point(&moored.f, BOAT_HALF_LEN - 0.15f, 0.2f, 0.0f, bow);
    glm_vec3_copy(bow, rope[0]);
    glm_vec3_copy(
        (vec3){0.5f * (bow[0] + DOCK_POST), LAKE_Y + 0.1f, 0.5f * (bow[2] + DOCK_Z - DOCK_HALF)},
        rope[1]);
    glm_vec3_copy((vec3){DOCK_POST, DOCK_Y + 0.35f, DOCK_Z - DOCK_HALF + 0.06f}, rope[2]);
    kit_frame_pipe(kit, &KIT_WORLD, MAT_POLE, rope, 3, 0.012f, 5);

    // Turned over on the bank north of the dock, above the water's edge, along the shore.
    const float bx = -74.6f, bz = 112.8f;
    const Boat beached = {{{bx, land_height(bx, bz) + 0.03f, bz}, 0.5f * GLM_PIf + 0.25f}, true};
    boat(kit, &beached, false);
    kit_collider(kit, (vec3){bx, beached.f.origin[1] + 0.25f, bz},
                 (vec3){BOAT_HALF_LEN + 0.05f, 0.25f, BOAT_HALF_BEAM + 0.05f}, beached.f.yaw);
}

/*
 * Reeds in the shallows and on the wet margin, in beds of a few clumps each, each clump two of
 * make_reeds.py's cards crossed through its foot. Not on the cabin's side, where the dock goes
 * out and the bank is kept clear to the water.
 */
static void reeds(Kit* kit, unsigned int* state) {
    static const float UV[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    for (int g = 0; g < REED_BEDS; g++) {
        const float theta = 2.0f * GLM_PIf * kit_xrnd(state);
        const float out = -2.4f + 3.0f * kit_xrnd(state);
        const int clumps = 2 + (int)(5.0f * kit_xrnd(state));
        float cx = 0.0f, cz = 0.0f;
        shore_at(theta, out, &cx, &cz);
        for (int c = 0; c < clumps; c++) {
            const float x = cx + 2.4f * (kit_xrnd(state) - 0.5f);
            const float z = cz + 2.4f * (kit_xrnd(state) - 0.5f);
            const float h = 1.0f + 0.7f * kit_xrnd(state), yaw = GLM_PIf * kit_xrnd(state);
            if (cosf(theta) > 0.75f)
                continue;
            const float w = 0.9f * h, y = land_height(x, z) - 0.05f;
            for (int k = 0; k < 2; k++) {
                const float a = yaw + 0.5f * GLM_PIf * (float)k;
                const vec3 across = {w * cosf(a), 0.0f, w * sinf(a)};
                const vec3 corner = {x - 0.5f * across[0], y, z - 0.5f * across[2]};
                kit_frame_card(kit, &KIT_WORLD, MAT_REEDS, corner, across, (vec3){0.0f, h, 0.0f},
                               UV);
            }
        }
    }
}

// Dead trees standing in the shallows on the far side, leaning as the drowned do.
static void drowned(Kit* kit, Trees* trees, unsigned int* state) {
    static const float BEARINGS[DROWNED_TREES] = {2.1f, 2.75f, 3.5f, 4.4f};
    for (int i = 0; i < DROWNED_TREES; i++) {
        const float theta = BEARINGS[i] + 0.15f * (kit_xrnd(state) - 0.5f);
        const float out = -1.8f - 1.6f * kit_xrnd(state);
        const float scale = 0.06f + 0.02f * kit_xrnd(state), yaw = 2.0f * GLM_PIf * kit_xrnd(state);
        const float lean = glm_rad(8.0f + 10.0f * kit_xrnd(state));
        const int which = (2 * i + 1) % TREE_MODELS;
        if (!trees->dead[which])
            continue;
        float x = 0.0f, z = 0.0f;
        shore_at(theta, out, &x, &z);
        trees_stand(trees, kit, which, x, z, scale, yaw, lean);
    }
}

void lake_build(Kit* kit, Scene* scene, Trees* trees, unsigned int seed, bool always_drawn) {
    unsigned int state = seed * 2246822519u + 0x1341eu;
    g_always_drawn = always_drawn;
    water(scene, always_drawn);
    ring(kit);
    dock(kit, &state);
    boats(kit);
    reeds(kit, &state);
    drowned(kit, trees, &state);
}

void lake_start_audio(AudioSystem* audio) {
    g_lapping = sounds_loop(audio, "assets/audio/silent/lake_lapping.flac");
    if (g_lapping)
        audio_sound_set_volume(g_lapping, LAPPING_VOLUME);
}

void lake_update(const Scene* scene, const float eye[3], const float feet[3]) {
    // One loop for the whole shore, where the eye is nearest to it, at the water's edge.
    if (g_lapping) {
        float x = 0.0f, z = 0.0f;
        shore_at(atan2f(eye[2] - LAKE_Z, eye[0] - LAKE_X), 0.0f, &x, &z);
        audio_sound_set_position(g_lapping, (vec3){x, LAKE_Y + 0.1f, z});
    }
    Water* w = scene->water;
    if (!w)
        return;
    if (!g_always_drawn)
        w->enabled = w->enabled ? eye[0] < WATER_OFF_X && eye[2] > WATER_OFF_Z
                                : eye[0] < WATER_ON_X && eye[2] > WATER_ON_Z;
    water_wake(w, &g_wake, feet[0], feet[2], WAKE_PACE, feet[1] < LAKE_Y);
}
