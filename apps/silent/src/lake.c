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
// Past this from the lake's centre the bowl has climbed back to the ground: the shore's widest,
// with its wobble, plus the bowl's longest reach, and a metre for rounding.
#define VALLEY_REACH (fmaxf(LAKE_RX, LAKE_RZ) * 1.12f + BOWL_REACH + BOWL_LONG + 1.0f)

// Metres out from the shore the bed reaches wading depth, where a ring of bodies stops anyone
// going further.
#define WADE_OUT 5.0f

#define SHELF_SLOPE 0.12f // the bed's fall per metre out from the shore: 0.6 m at the wading ring
#define DROP        3.0f  // and then down, between DROP_FROM and DROP_TO metres out
#define DROP_FROM   WADE_OUT
#define DROP_TO     14.0f
#define FLOOR_FALL  0.6f // a last fall to the deepest water, out to FLOOR_TO
#define FLOOR_TO    30.0f
#define FLOOR_LUMPS 0.25f // metres of lumps on the floor

#define PAD_SHOULDER 3.0f // metres over which the ground comes to the pad's level

#define TRACK_ASPHALT 10.0f // metres of the cross street's south arm the track starts along

// The track's line: on down the cross street's south arm, through the old cutting, round the
// ridge's east end, and down the bank to the cabin's pad.
static const float TRACK_POINTS[][2] = {
    {CROSS_X, CROSS_Z1 - TRACK_ASPHALT},
    {CROSS_X, CROSS_Z1},
    {-51.0f, 48.0f},
    {-50.0f, 60.0f},
    {-54.0f, 72.0f},
    {-63.0f, 80.0f},
    {-73.0f, 88.0f},
    {-77.0f, 98.0f},
    {-71.0f, 106.0f},
    {-65.0f, 113.0f},
};

// Level over the asphalt it starts on and at the pad, between them one grade, under the 8 degrees
// puddles stand on, eased in and out over ten metres.
static const RoadDesc TRACK = {
    .points = TRACK_POINTS,
    .point_count = (int)(sizeof(TRACK_POINTS) / sizeof(TRACK_POINTS[0])),
    .half = TRACK_HALF,
    .shoulder = 4.0f,
    .y0 = ROAD_Y,
    .y1 = CABIN_PAD_Y,
    .flat_run = TRACK_ASPHALT,
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

// How far (x, z) is from the shoreline in plan, and its bearing from the lake's centre.
static float shore_offset(float x, float z, float* theta) {
    const float dx = x - LAKE_X, dz = z - LAKE_Z;
    *theta = atan2f(dz, dx);
    return hypotf(dx, dz) - shore_radius(*theta);
}

float lake_shore_distance(float x, float z) {
    float theta = 0.0f;
    return shore_offset(x, z, &theta);
}

float lake_track_distance(float x, float z) {
    return road_distance(&g_track, x, z, NULL);
}

void lake_track_point(float t, float* x, float* z) {
    road_point(&g_track, t, x, z);
}

// The valley's own shape, before the track and the pad: the ridge, the bowl and the bed.
static float valley(float x, float z, float h) {
    const float ridge = RIDGE_RISE *
                        (1.0f - glm_smoothstep(0.0f, RIDGE_HALF, fabsf(z - RIDGE_CREST_Z))) *
                        glm_smoothstep(RIDGE_X0, RIDGE_X1, x);
    if (ridge > 0.0f)
        h += ridge;
    const float dx = x - LAKE_X, dz = z - LAKE_Z;
    if (dx * dx + dz * dz >= VALLEY_REACH * VALLEY_REACH)
        return h;
    float theta = 0.0f;
    const float d = shore_offset(x, z, &theta);
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
 * water, and nothing below LAKE_Y elsewhere -- the chasm -- floods or darkens. Its surface, the
 * light it takes from what is under it and its ripples cost a frame wherever the eye is, so it
 * is on only within a reach of the eye that the fog hides; in clear air, always.
 *
 * Dark and still: a peat lake in the woods, tea-brown and opaque at arm's length, under a breath
 * of wind. No caustics and no motes, which such water would not show.
 *
 * Its waves are the spectral sea's (spec 13.56), with no swell, which needs an ocean. The wind
 * sea is livelier than the lake's own 40 m of fetch would make it: 3.5 m/s over that seeds 14 cm
 * ripples, Hs 2 cm, which read as still water from the dock. This seeds 0.6 s wavelets, Hs 7 cm,
 * a breeze ruffling the whole lake. They lie in the bands that only shade, so the surface itself
 * barely moves.
 */

#define WATER_X0 (-124.0f) // where the water is: round the basin, its edges under the bank
#define WATER_X1 (-68.0f)
#define WATER_Z0 97.0f
#define WATER_Z1 141.0f

#define LAKE_SEA_DEPTH  4.0f   // metres, the bed's past the drop-off
#define LAKE_WIND_SPEED 5.0f   // m/s at the 10 m reference height
#define LAKE_FETCH      150.0f // metres of open water the wind has crossed

// The wading ring's bodies, from under the bed at the ring to over a head, and half their
// thickness; on chords of the curve, they stand a few centimetres further out than it.
#define RING_FOOT (LAKE_Y - 0.9f)
#define RING_TOP  (LAKE_Y + 1.6f)
#define RING_HALF 0.15f
#define RING_SAG  0.1f
#define WAKE_PACE 0.6f // metres a wader moves for each ripple

#define DOCK_X0   (-72.0f)                       // where its deck leaves the bank
#define DOCK_X1   (-85.0f)                       // and its far end, over deep water
#define DOCK_Z    (0.5f * (CABIN_Z0 + CABIN_Z1)) // its centre line, out from the cabin's door
#define DOCK_HALF 0.8f                           // half its width
#define DOCK_Y    (LAKE_Y + 0.95f)               // the deck's top, a metre over the water
#define DOCK_POST (DOCK_X1 + 0.3f) // the last pair of posts, standing up past the deck

// The dock lamp's post, on the deck's south edge over the moored boat.
#define LAMP_X (DOCK_X1 + 1.5f)
#define LAMP_Z (DOCK_Z - DOCK_HALF + 0.1f)

#define REED_BEDS     18 // clumps of clumps
#define DROWNED_TREES 4

// Against the other loops tools/fetch_sounds.py levels alike.
#define LAPPING_VOLUME 0.35f

// The point `out` metres outside the shore (negative: over the water) on bearing `theta`.
static void shore_at(float theta, float out, float* x, float* z) {
    const float r = shore_radius(theta) + out;
    *x = LAKE_X + r * cosf(theta);
    *z = LAKE_Z + r * sinf(theta);
}

void lake_shore_point(float theta, float* x, float* z) {
    shore_at(theta, 0.0f, x, z);
}

static Water* water(Scene* scene, WaterWaveModel waves) {
    Water* w = create_water();
    if (!w)
        return NULL;
    w->level = LAKE_Y;
    glm_vec4_copy((vec4){WATER_X0, WATER_Z0, WATER_X1, WATER_Z1}, w->domain.bounds);
    // No bed: the surface reads its column off the scene's depth, which the basin is, and a bed
    // would only bring a shore's surf to a lake with none.
    w->height_at = NULL;
    w->wave_model = waves;
    // The street's breeze, down the valley.
    glm_vec2_copy((vec2){0.0f, 1.0f}, w->wind_dir);
    // A lake's own sea: the breeze over the water's length, and no swell, which needs an ocean.
    w->sea.sea_depth = LAKE_SEA_DEPTH;
    w->sea.wind_sea.wind_speed = LAKE_WIND_SPEED;
    w->sea.wind_sea.fetch = LAKE_FETCH;
    w->sea.swell.scale = 0.0f;
    w->amplitude = 0.015f;
    w->wavelength = 3.0f;
    w->steepness = 0.3f;
    glm_vec3_copy((vec3){1.0f, 1.4f, 2.2f}, w->absorption);
    glm_vec3_copy((vec3){0.002f, 0.003f, 0.003f}, w->scatter_albedo);
    w->caustics = false;
    w->specks = false;
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
    return w;
}

// The ring's bearing where it crosses the line z on the east side, where the dock goes out.
static float ring_bearing(float z) {
    float lo = -0.8f, hi = 0.8f;
    for (int k = 0; k < 30; k++) {
        const float mid = 0.5f * (lo + hi);
        float x = 0.0f, zm = 0.0f;
        shore_at(mid, -WADE_OUT, &x, &zm);
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
    shore_at(from, -WADE_OUT, &px, &pz);
    for (int k = 1; k <= chords; k++) {
        const float t = from + (to - from) * (float)k / (float)chords;
        float x = 0.0f, z = 0.0f;
        shore_at(t, -WADE_OUT, &x, &z);
        const float dx = x - px, dz = z - pz;
        kit_collider(kit, (vec3){0.5f * (x + px), 0.5f * (RING_TOP + RING_FOOT), 0.5f * (z + pz)},
                     (vec3){RING_HALF, 0.5f * (RING_TOP - RING_FOOT), 0.5f * hypotf(dx, dz) + 0.1f},
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
    kit_frame_floor(kit, &KIT_WORLD, MAT_PORCH, DOCK_X0, DOCK_X1, DOCK_Y - 0.4f, DOCK_Y, z0, z1);
    // From inside the ring out, down to below the bed and over a head.
    float ring_x = 0.0f, ring_z = 0.0f;
    shore_at(0.0f, -WADE_OUT, &ring_x, &ring_z);
    const float bx0 = ring_x + 1.0f, bx1 = DOCK_X1 - 0.3f, foot = LAKE_Y - 4.5f,
                head = DOCK_Y + 1.0f;
    const float yc = 0.5f * (foot + head), yh = 0.5f * (head - foot);
    for (int s = 0; s < 2; s++)
        kit_collider(kit, (vec3){0.5f * (bx0 + bx1), yc, s ? z1 + 0.15f : z0 - 0.15f},
                     (vec3){0.5f * (bx0 - bx1), yh, 0.15f}, 0.0f);
    kit_collider(kit, (vec3){bx1 - 0.15f, yc, DOCK_Z}, (vec3){0.15f, yh, DOCK_HALF + 0.3f}, 0.0f);
}

/*
 * Where the boats are kept (spec 13.56). The motorboat moors off the dock's end on its south side,
 * its bow to the tall post. The bodies down the dock's side stand 0.6 m off its gunwale. Its
 * twin lies turned over on the bank north of the dock, along the shore.
 */
#define MOOR_X       (DOCK_X1 + 2.0f)
#define MOOR_Z       (DOCK_Z - DOCK_HALF - 0.95f)
#define MOOR_HEADING (-0.5f * GLM_PIf - 0.06f) // the bow toward -x, at the post
#define BANK_X       (-74.6f)
#define BANK_Z       112.8f
#define BANK_HEADING (-2.89f) // the bow up the shore, a little out from it

void lake_berths(LakeBerths* out) {
    float ring_x = 0.0f, ring_z = 0.0f;
    shore_at(0.0f, -WADE_OUT, &ring_x, &ring_z);
    *out = (LakeBerths){
        .moored = {{MOOR_X, LAKE_Y, MOOR_Z}, MOOR_HEADING},
        .post = {DOCK_POST, DOCK_Y + 0.35f, DOCK_Z - DOCK_HALF + 0.06f},
        // A stride in from the deck's south edge, level with the boat's middle.
        .landing = {MOOR_X, DOCK_Y, DOCK_Z - DOCK_HALF + 0.45f},
        .landing_yaw = 0.5f * GLM_PIf,
        // The deck, and the bodies down its sides and across its end.
        .dock = {DOCK_X1 - 0.45f, DOCK_Z - DOCK_HALF - 0.3f, ring_x + 1.0f,
                 DOCK_Z + DOCK_HALF + 0.3f},
        .bank = {{BANK_X, land_height(BANK_X, BANK_Z), BANK_Z}, BANK_HEADING},
        .lamp = {{LAMP_X, DOCK_Y, LAMP_Z}, GLM_PIf},
    };
}

float lake_open_water(float x, float z) {
    return -lake_shore_distance(x, z) - WADE_OUT - RING_HALF - RING_SAG;
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

void lake_build(Lake* lake, Kit* kit, Scene* scene, Trees* trees, unsigned int seed, float reach,
                WaterWaveModel waves) {
    *lake = (Lake){.water = water(scene, waves), .reach = reach};
    unsigned int state = seed * 2246822519u + 0x1341eu;
    ring(kit);
    dock(kit, &state);
    reeds(kit, &state);
    drowned(kit, trees, &state);
}

// One loop for the whole shore, at the water's edge on bearing `theta` from the lake's centre.
static void lapping_at(const Lake* lake, float theta) {
    float x = 0.0f, z = 0.0f;
    shore_at(theta, 0.0f, &x, &z);
    audio_sound_set_position(lake->lapping, (vec3){x, LAKE_Y + 0.1f, z});
}

void lake_start_audio(Lake* lake, AudioSystem* audio) {
    lake->lapping = sounds_loop(audio, "assets/audio/silent/lake_lapping.flac", AUDIO_BUS_SFX);
    if (!lake->lapping)
        return;
    lapping_at(lake, 0.0f);
    audio_sound_set_volume(lake->lapping, LAPPING_VOLUME);
}

void lake_update(Lake* lake, const float eye[3], const float feet[3]) {
    if (lake->lapping)
        lapping_at(lake, atan2f(eye[2] - LAKE_Z, eye[0] - LAKE_X));
    if (!lake->water)
        return;
    lake->water->enabled =
        lake->reach <= 0.0f ||
        plan_box_distance(eye[0], eye[2], WATER_X0, WATER_X1, WATER_Z0, WATER_Z1) < lake->reach;
    // Wading where the water is, which the inside of a boat floating on it is not.
    water_wake(lake->water, &lake->wake, feet[0], feet[2], WAKE_PACE,
               water_under(LAKE_Y, &lake->water->domain, feet[0], feet[1], feet[2]));
}
