#include <math.h>
#include <stdio.h>

#include "cetra/light.h"

#include "basement.h"
#include "home.h"
#include "kitchen.h"
#include "lights.h"
#include "mats.h"

#define SLAB_DEPTH  0.2f // the slab, under BASEMENT_Y
#define SILL_W      0.1f // the sill plate the framing sits on, along the walls' tops
#define JOIST_W     0.045f
#define JOIST_PITCH 0.4f
#define JOIST_LAP   0.1f    // how far a joist runs on past the beam's middle, beside its neighbour
#define BEAM_X      HALL_X1 // the beam's middle, under the hall's east wall
#define BEAM_HALF   0.065f
#define BEAM_Y0     (-0.18f)
#define FLANGE      0.015f
#define POST_R      0.045f

// The flight down: steep, as a cellar's is, every riser under the player's step, so its foot
// leaves a metre of floor before the wall ahead to turn in.
#define CELLAR_RISERS 12
#define CELLAR_RISE   ((FLOOR_Y - BASEMENT_Y) / (float)CELLAR_RISERS)
#define CELLAR_GOING  0.21f
#define CELLAR_FOOT_X (CELLAR_HEAD_X - (float)(CELLAR_RISERS - 1) * CELLAR_GOING)
#define TREAD_T       0.035f
#define NOSING        0.02f
#define STRINGER_W    0.05f
#define STRINGER_UP 0.015f  // its top edge over the line of the nosings: under the open door's leaf
#define STRINGER_DOWN 0.26f // and its foot under it
#define RAIL_H        0.86f // a handrail's top over the nosings
#define RAIL_T        0.035f
#define RAIL_D        0.07f
#define RAIL_BRACKETS 3

// The stairwell's dark band over its paint, along the flight over its nosings and level across
// the wall at its foot.
#define DADO_H (1.0f)
#define DADO_Y (BASEMENT_Y + DADO_H)
#define DADO_T 0.002f
// How high the damp has climbed the basement's own walls.
#define DAMP_Y (BASEMENT_Y + 0.6f)
// The way from the stairwell's foot into the basement, on the right at the bottom: the whole
// width of the floor there, and a doorway's height under a header.
#define FOOT_HEAD (BASEMENT_Y + 2.1f)
static const KitOpening WAY_IN = {CELLAR_X0, CELLAR_FOOT_X, BASEMENT_Y, FOOT_HEAD, .door = true};
// Feet this far over the basement's floor, or nearer, are at the flight's foot: its last two
// steps and the floor below them. Feet over HALL_NEAR are back up near the hall.
#define FOOT_REACH (2.0f * CELLAR_RISE + 0.05f)
#define HALL_NEAR  (FLOOR_Y - 0.5f)

// The bulb: on a cord from a rose on the stairwell's ceiling over the upper flight, low enough
// that the door opens on it at a standing eye's height. Lengths from the pivot under the rose.
#define BULB_X       (-2.75f)
#define BULB_Z       (0.5f * (STAIRWELL_Z0 + CELLAR_Z1))
#define ROSE_DROP    0.026f
#define CORD_L       0.82f
#define GLASS_FOOT   1.01f
#define GLASS_WIDEST 0.034f                      // over the glass's foot
#define BULB_DROP    (GLASS_FOOT - GLASS_WIDEST) // to the glass's widest, where the light is
#define BULB_CANDELA 30.0f
#define BULB_RANGE   6.0f
#define BULB_NITS    1500.0f
// Its filament's colour at full strength, and in the depth of a brownout.
static const vec3 BULB_COLOUR = {1.0f, 0.78f, 0.52f};
static const vec3 BULB_SAGGED = {1.0f, 0.55f, 0.25f};
// The draft's swing the first time the door opens far enough: along the flight, pushed west,
// away from the door, a little across it, dying away; and the faint sway it leaves.
#define DRAFT_OPEN  0.25f // how far open the door lets the draft in, as Door.travel
#define DRAFT_SWING 0.16f // radians
#define DRAFT_SIDE  0.05f
#define DRAFT_DECAY 3.5f // seconds
#define SWAY        0.012f
// While the swing still carries the glass further than this, the bulb's kept shadow follows it at
// any move. The sway left after it stays under the shadows' tolerance and costs no redraw.
#define FOLLOW_REACH 0.015f

// The tap's drip (tools/fetch_sounds.py): seconds to the first, the least between two and how
// much more at random, and its level against the house's other sounds.
#define DRIP_PATH   "assets/audio/silent/basement_drip.wav"
#define DRIP_FIRST  2.0
#define DRIP_EVERY  2.6
#define DRIP_SPREAD 1.6
#define DRIP_VOLUME 0.5f

// The posts under the beam, between the irradiance probes' rows at every half metre of z.
static const float POSTS_Z[] = {12.0f, 15.0f, 18.0f};

// A wall of the basement from y0 to y1: bare concrete, dark with damp to the tide line. It
// collides.
static void cellar_wall(Kit* kit, float x0, float x1, float y0, float y1, float z0, float z1) {
    if (y0 < DAMP_Y)
        kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_DAMP, x0, x1, y0, fminf(DAMP_Y, y1), z0, z1,
                      true);
    if (y1 > DAMP_Y)
        kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_CONCRETE, x0, x1, fmaxf(DAMP_Y, y0), y1, z0, z1,
                      true);
}

// The foundation from the slab's foot to the sill, the house's outside walls standing on its
// top. And the slab.
static void foundation(Kit* kit) {
    const float foot = BASEMENT_Y - SLAB_DEPTH;
    cellar_wall(kit, DIG_X0, DIG_X1, foot, 0.0f, DIG_Z0, CELLAR_Z0);
    cellar_wall(kit, DIG_X0, DIG_X1, foot, 0.0f, CELLAR_Z1, DIG_Z1);
    cellar_wall(kit, DIG_X0, CELLAR_X0, foot, 0.0f, CELLAR_Z0, CELLAR_Z1);
    cellar_wall(kit, CELLAR_X1, DIG_X1, foot, 0.0f, CELLAR_Z0, CELLAR_Z1);
    kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_FLOOR, CELLAR_X0, CELLAR_X1, foot, BASEMENT_Y,
                  CELLAR_Z0, CELLAR_Z1, true);
}

// A piece of the floor's framing, from x0 to x1, y0 to y1 and z0 to z1, drawn only where it is
// seen from below: the boards cover the rest.
static void timber(Kit* kit, float x0, float x1, float y0, float y1, float z0, float z1) {
    kit_frame_box_faces(kit, &KIT_WORLD, MAT_JOIST, x0, x1, y0, y1, z0, z1,
                        KIT_FACES_ALL & ~KIT_FACE_UP);
}

// A ring of timbers `w` wide along the walls' tops from y0 to y1, the back one and the west one
// stopping at the stairwell, the west at its trimmer.
static void ring(Kit* kit, float y0, float y1, float w, float trimmer) {
    timber(kit, CELLAR_X0, CELLAR_X1, y0, y1, CELLAR_Z0, CELLAR_Z0 + w);
    timber(kit, CELLAR_HEAD_X, CELLAR_X1, y0, y1, CELLAR_Z1 - w, CELLAR_Z1);
    timber(kit, CELLAR_X0, CELLAR_X0 + w, y0, y1, CELLAR_Z0 + w, trimmer);
    timber(kit, CELLAR_X1 - w, CELLAR_X1, y0, y1, CELLAR_Z0 + w, CELLAR_Z1 - w);
}

/*
 * The ground floor's framing, which is the basement's ceiling: sill plates on the walls, a rim
 * joist round them, the joists across from each wall to the beam, lapped past each other over
 * it, and the subfloor on them. Round the stairwell the joists it cuts hang from a header at the
 * head of the flight, and a doubled trimmer runs along its side under the partition.
 */
static void framing(Kit* kit) {
    const float j0 = JOIST_Y0, j1 = SUBFLOOR_Y0, r = JOIST_W;
    const float trimmer = STAIRWELL_Z0 - 2.0f * r, header = CELLAR_HEAD_X + 2.0f * r;
    ring(kit, 0.0f, j0, SILL_W, trimmer);
    ring(kit, j0, j1, r, trimmer);

    // The stairwell's trimmer and header.
    const float west = BEAM_X + JOIST_LAP, east = BEAM_X - JOIST_LAP;
    timber(kit, CELLAR_X0, west, j0, j1, trimmer, STAIRWELL_Z0);
    timber(kit, CELLAR_HEAD_X, header, j0, j1, STAIRWELL_Z0, CELLAR_Z1 - r);

    // The joists: the west ones at each pitch, the east ones a joist's width on, lapped.
    for (float z = CELLAR_Z0 + 0.5f * JOIST_PITCH; z + 1.5f * r < CELLAR_Z1 - r; z += JOIST_PITCH) {
        if (z + 0.5f * r < trimmer)
            timber(kit, CELLAR_X0 + r, west, j0, j1, z - 0.5f * r, z + 0.5f * r);
        else if (z - 0.5f * r > STAIRWELL_Z0)
            timber(kit, header, west, j0, j1, z - 0.5f * r, z + 0.5f * r);
        timber(kit, east, CELLAR_X1 - r, j0, j1, z + 0.5f * r, z + 1.5f * r);
    }

    // The subfloor, round the stairwell.
    const float sub1 = FLOOR_Y - FLOOR_BOARDS;
    timber(kit, CELLAR_X0, CELLAR_X1, j1, sub1, CELLAR_Z0, STAIRWELL_Z0);
    timber(kit, CELLAR_HEAD_X, CELLAR_X1, j1, sub1, STAIRWELL_Z0, CELLAR_Z1);
}

// A steel beam under the middle of the house, where the hall's east wall stands over it, on
// three lally columns.
static void beam(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x = BEAM_X, b = BEAM_HALF, top = JOIST_Y0;
    kit_frame_box(kit, w, MAT_ENAMEL, x - b, x + b, top - FLANGE, top, CELLAR_Z0, CELLAR_Z1, false);
    kit_frame_box(kit, w, MAT_ENAMEL, x - 0.005f, x + 0.005f, BEAM_Y0 + FLANGE, top - FLANGE,
                  CELLAR_Z0, CELLAR_Z1, false);
    kit_frame_box(kit, w, MAT_ENAMEL, x - b, x + b, BEAM_Y0, BEAM_Y0 + FLANGE, CELLAR_Z0, CELLAR_Z1,
                  false);
    for (int i = 0; i < KIT_COUNT(POSTS_Z); i++) {
        const float z = POSTS_Z[i], p = 0.08f;
        kit_prism(kit, MAT_ENAMEL, x, z, BASEMENT_Y + 0.012f, BEAM_Y0 - 0.012f, POST_R, 10, true);
        kit_frame_box(kit, w, MAT_ENAMEL, x - p, x + p, BEAM_Y0 - 0.012f, BEAM_Y0, z - p, z + p,
                      false);
        kit_frame_box(kit, w, MAT_ENAMEL, x - p, x + p, BASEMENT_Y, BASEMENT_Y + 0.012f, z - p,
                      z + p, false);
    }
}

// The height of the flight's pitch at x: the line through every tread's front edge.
static float nosing_y(float x) {
    return FLOOR_Y + (x - CELLAR_HEAD_X) * (CELLAR_RISE / CELLAR_GOING);
}

// And where along it the pitch is at height y.
static float nosing_x(float y) {
    return CELLAR_HEAD_X + (y - FLOOR_Y) * (CELLAR_GOING / CELLAR_RISE);
}

// A rail of the pitch from x0 to x1, its top RAIL_H over the nosings, from z0 to z1.
static void raked_rail(Kit* kit, float x0, float x1, float z0, float z1) {
    const vec2 rail[4] = {{x0, nosing_y(x0) + RAIL_H},
                          {x1, nosing_y(x1) + RAIL_H},
                          {x1, nosing_y(x1) + RAIL_H - RAIL_D},
                          {x0, nosing_y(x0) + RAIL_H - RAIL_D}};
    kit_frame_extrude(kit, &KIT_WORLD, MAT_JOIST, rail, 4, z0, z1);
}

/*
 * The stair down: a steep cellar flight going west from the door's threshold, which the door
 * swings in over, open treads housed between two stringers, walled in on both sides, the last
 * riser landing short of the wall ahead. Each tread's body is one rise deep, so what is under the
 * flight is not filled in.
 */
static void stair(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float z0 = STAIRWELL_Z0 + STRINGER_W, z1 = CELLAR_Z1 - STRINGER_W;
    for (int k = 1; k < CELLAR_RISERS; k++) {
        const float y = FLOOR_Y - (float)k * CELLAR_RISE;
        const float x1 = CELLAR_HEAD_X - (float)(k - 1) * CELLAR_GOING, x0 = x1 - CELLAR_GOING;
        kit_frame_box(kit, w, MAT_WOOD, x0 - NOSING, x1, y - TREAD_T, y, z0, z1, false);
        kit_frame_box(kit, w, KIT_COLLIDER_ONLY, x0, x1, y - CELLAR_RISE, y, z0, z1, true);
    }

    // The stringers, cut plumb at the last riser and level on the floor.
    const float foot = CELLAR_FOOT_X, heel = nosing_x(BASEMENT_Y + STRINGER_DOWN);
    const vec2 stringer[5] = {{CELLAR_HEAD_X, nosing_y(CELLAR_HEAD_X) + STRINGER_UP},
                              {foot, nosing_y(foot) + STRINGER_UP},
                              {foot, BASEMENT_Y},
                              {heel, BASEMENT_Y},
                              {CELLAR_HEAD_X, nosing_y(CELLAR_HEAD_X) - STRINGER_DOWN}};
    kit_frame_extrude(kit, w, MAT_JOIST, stringer, 5, STAIRWELL_Z0, z0);
    kit_frame_extrude(kit, w, MAT_JOIST, stringer, 5, z1, CELLAR_Z1);

    // A handrail on the back wall, on iron brackets spaced evenly along it.
    const float rz = CELLAR_Z1 - 0.06f, top = CELLAR_HEAD_X - 0.15f, bottom = foot - 0.05f;
    raked_rail(kit, top, bottom, rz - RAIL_T, rz);
    for (int i = 0; i < RAIL_BRACKETS; i++) {
        const float x = top + (bottom - top) * ((float)i + 0.5f) / (float)RAIL_BRACKETS;
        const float under = nosing_y(x) + RAIL_H - RAIL_D;
        kit_frame_box(kit, w, MAT_IRON, x - 0.012f, x + 0.012f, under - 0.03f, under,
                      rz - 0.5f * RAIL_T, CELLAR_Z1, false);
    }
}

// The dark band over the paint on frame `f`: the outline in (a, y), just proud of the paint's face
// at d, toward `out`.
static void dado(Kit* kit, const KitFrame* f, const vec2* outline, int count, float d, float out) {
    kit_frame_extrude(kit, f, MAT_CELLAR_DADO, outline, count, d, d + out * DADO_T);
}

/*
 * The stairwell, walled in from the door to the foot and painted, so what the door opens on is a
 * narrow way down between two walls and a third ahead. Under the floor its south wall stands on
 * the slab under the partition and stops at the last riser: the floor beyond it, a metre to the
 * wall ahead, is the way into the basement, to the right at the bottom, under a header. Its east
 * end closes it under the head of the flight. Both are the basement's concrete on its side. Every
 * face inside is one coat of the pale paint from the slab to the ceiling, over the plaster above
 * the floor and the concrete below alike, but the east end under the floor, which is all dark
 * band; and the band runs down the two long walls along the flight, DADO_H over its nosings, and
 * level across the wall at its foot.
 */
static void stairwell(Kit* kit) {
    const float s0 = STAIRWELL_WALL_Z - 0.5f * INT_WALL, hall = HALL_OUT_X0;
    const float foot = CELLAR_FOOT_X;
    cellar_wall(kit, foot, HALL_X0, BASEMENT_Y, JOIST_Y0, s0, STAIRWELL_Z0);
    cellar_wall(kit, CELLAR_HEAD_X, HALL_X0, BASEMENT_Y, JOIST_Y0, STAIRWELL_Z0, CELLAR_Z1);
    kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_CONCRETE, CELLAR_X0, foot, FOOT_HEAD, JOIST_Y0, s0,
                  STAIRWELL_Z0, true);

    home_line_stairwell(kit, MAT_CELLAR_WALL, &WAY_IN);
    kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_DADO, CELLAR_HEAD_X - LINING, CELLAR_HEAD_X,
                  BASEMENT_Y, FLOOR_Y, STAIRWELL_Z0, CELLAR_Z1, false);

    // The band: down the flight's rake to where the nosings would meet the floor, and level on.
    const float level = nosing_x(BASEMENT_Y);
    const vec2 north[5] = {{hall, nosing_y(hall) + DADO_H},
                           {level, DADO_Y},
                           {CELLAR_X0, DADO_Y},
                           {CELLAR_X0, BASEMENT_Y},
                           {hall, BASEMENT_Y}};
    dado(kit, &KIT_WORLD, north, 5, CELLAR_Z1 - LINING, -1.0f);
    const vec2 south[4] = {{hall, nosing_y(hall) + DADO_H},
                           {foot, nosing_y(foot) + DADO_H},
                           {foot, BASEMENT_Y},
                           {hall, BASEMENT_Y}};
    dado(kit, &KIT_WORLD, south, 4, STAIRWELL_Z0 + LINING, 1.0f);
    const vec2 west[4] = {{STAIRWELL_Z0, BASEMENT_Y},
                          {CELLAR_Z1, BASEMENT_Y},
                          {CELLAR_Z1, DADO_Y},
                          {STAIRWELL_Z0, DADO_Y}};
    dado(kit, &KIT_WORLD_Z, west, 4, -(CELLAR_X0 + LINING), -1.0f);
}

// ---------------------------------------------------------------------------------------------
// What is down there

#define FURNACE_X (-3.0f)
#define FURNACE_Z 13.0f
#define HEATER_X  (-4.45f)
#define HEATER_Z  10.65f
#define DUCT_R    0.1f
#define DUCT_Y    (-0.15f) // where the ducts run, under the joists
#define FLUE_Y    (-0.4f)

/*
 * An old coal furnace, the octopus kind: a squat round body, its feed and ash doors sooted round,
 * and its ducts reaching up and out from the crown to run under the joists to the rooms above,
 * never due north or south, where two of the irradiance probes stand over it. The body is wide
 * enough to hold two more inside it, which switches them off rather than leaving them a hand's
 * breadth from it. Its flue goes back to the chimney through the west wall.
 */
static void furnace(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const vec2 body[] = {{0.0f, 0.0f},  {0.6f, 0.0f},  {0.63f, 0.05f}, {0.63f, 0.12f},
                         {0.6f, 0.15f}, {0.6f, 0.95f}, {0.63f, 0.98f}, {0.63f, 1.05f},
                         {0.52f, 1.2f}, {0.3f, 1.32f}, {0.14f, 1.37f}, {0.0f, 1.38f}};
    kit_frame_lathe(kit, w, MAT_LAMP_POST, FURNACE_X, FURNACE_Z, BASEMENT_Y, body, KIT_COUNT(body),
                    20);

    // The doors, facing into the room: the feed door and under it the ash door, each in a
    // blackened patch.
    const KitFrame front = {{FURNACE_X, BASEMENT_Y, FURNACE_Z}, 0.5f * GLM_PIf};
    kit_frame_box(kit, &front, MAT_SOOT, -0.24f, 0.24f, 0.4f, 0.8f, 0.55f, 0.615f, false);
    kit_frame_box(kit, &front, MAT_IRON, -0.17f, 0.17f, 0.47f, 0.73f, 0.6f, 0.64f, false);
    kit_frame_box(kit, &front, MAT_IRON, 0.1f, 0.15f, 0.58f, 0.62f, 0.64f, 0.68f, false);
    kit_frame_box(kit, &front, MAT_SOOT, -0.2f, 0.2f, 0.1f, 0.34f, 0.57f, 0.635f, false);
    kit_frame_box(kit, &front, MAT_IRON, -0.15f, 0.15f, 0.14f, 0.3f, 0.62f, 0.65f, false);

    const float crown = BASEMENT_Y + 1.33f;
    const struct {
        float deg, reach;
    } ARMS[] = {{0.0f, 2.3f}, {50.0f, 2.0f}, {130.0f, 1.8f}, {230.0f, 1.8f}, {310.0f, 2.0f}};
    for (int i = 0; i < KIT_COUNT(ARMS); i++) {
        const float c = cosf(glm_rad(ARMS[i].deg)), s = sinf(glm_rad(ARMS[i].deg));
        const float r[7] = {
            0.22f, 0.45f, 0.7f, 0.9f, ARMS[i].reach, ARMS[i].reach + 0.12f, ARMS[i].reach + 0.14f};
        const float y[7] = {crown, crown + 0.3f, DUCT_Y - 0.12f, DUCT_Y, DUCT_Y, 0.0f, 0.2f};
        vec3 path[7];
        for (int k = 0; k < 7; k++)
            glm_vec3_copy((vec3){FURNACE_X + r[k] * c, y[k], FURNACE_Z + r[k] * s}, path[k]);
        kit_frame_pipe(kit, w, MAT_STEEL, path, 7, DUCT_R, 12);
    }
    const vec3 flue[4] = {{FURNACE_X - 0.2f, crown - 0.05f, FURNACE_Z},
                          {FURNACE_X - 0.35f, FLUE_Y - 0.2f, FURNACE_Z},
                          {FURNACE_X - 0.6f, FLUE_Y, FURNACE_Z},
                          {CELLAR_X0, FLUE_Y, FURNACE_Z}};
    kit_frame_pipe(kit, w, MAT_IRON, flue, 4, 0.085f, 10);
    const vec2 thimble[] = {{0.0f, 0.0f}, {0.13f, 0.0f}, {0.13f, 0.025f}, {0.0f, 0.025f}};
    kit_frame_lathe_on(kit, w, MAT_IRON, (vec3){CELLAR_X0, FLUE_Y, FURNACE_Z},
                       (vec3){1.0f, 0.0f, 0.0f}, thimble, KIT_COUNT(thimble), 12);

    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, FURNACE_X - 0.66f, FURNACE_X + 0.66f, BASEMENT_Y,
                  crown + 0.1f, FURNACE_Z - 0.66f, FURNACE_Z + 0.66f, true);
}

// A tall water heater in the south-west corner, its flue and its two pipes going up into the
// floor.
static void water_heater(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x = HEATER_X, z = HEATER_Z, foot = BASEMENT_Y + 0.08f, top = foot + 1.55f;
    const vec2 stand[] = {{0.0f, 0.0f}, {0.2f, 0.0f}, {0.2f, 0.08f}, {0.0f, 0.08f}};
    kit_frame_lathe(kit, w, MAT_IRON, x, z, BASEMENT_Y, stand, KIT_COUNT(stand), 12);
    const vec2 tank[] = {{0.0f, 0.0f},  {0.24f, 0.0f},  {0.25f, 0.02f}, {0.25f, 1.45f},
                         {0.2f, 1.52f}, {0.06f, 1.55f}, {0.0f, 1.55f}};
    kit_frame_lathe(kit, w, MAT_APPLIANCE, x, z, foot, tank, KIT_COUNT(tank), 18);
    const vec3 flue[2] = {{x, top - 0.02f, z}, {x, 0.2f, z}};
    kit_frame_pipe(kit, w, MAT_STEEL, flue, 2, 0.05f, 10);
    for (int i = -1; i <= 1; i += 2) {
        const vec3 pipe[2] = {{x + 0.12f * (float)i, top - 0.06f, z},
                              {x + 0.12f * (float)i, 0.2f, z}};
        kit_frame_pipe(kit, w, MAT_BRASS, pipe, 2, 0.012f, 8);
    }
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, x - 0.25f, x + 0.25f, BASEMENT_Y, top, z - 0.25f,
                  z + 0.25f, true);
}

// A double laundry tub on iron legs against the east wall, one side holding grey water that has
// stood there a long time, under a tap from the wall.
#define TUB_Z 14.0f
static const KitFrame TUB = {{CELLAR_X1, BASEMENT_Y, TUB_Z}, -0.5f * GLM_PIf}; // d into the room
// The tap's mouth, over the side that holds water, and that water's top, in the tub's frame:
// where the drip falls from and lands.
#define TAP_A     (-0.27f)
#define TAP_D     0.31f
#define TAP_Y     1.02f
#define TUB_WATER 0.77f

static void laundry_tub(Kit* kit) {
    const KitFrame f = TUB;
    const float ha = 0.55f, d0 = 0.02f, d1 = 0.6f, y0 = 0.55f, y1 = 0.9f, t = 0.04f;
    kit_frame_box(kit, &f, MAT_APPLIANCE, -ha, ha, y0, y0 + t, d0, d1, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, -ha, ha, y0, y1, d1 - t, d1, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, -ha, ha, y0, y1, d0, d0 + t, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, -ha, -ha + t, y0, y1, d0 + t, d1 - t, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, ha - t, ha, y0, y1, d0 + t, d1 - t, false);
    kit_frame_box(kit, &f, MAT_APPLIANCE, -0.02f, 0.02f, y0, y1, d0 + t, d1 - t, false);
    kit_frame_box(kit, &f, MAT_DARK_GLASS, -ha + t, -0.02f, y0 + t, TUB_WATER, d0 + t, d1 - t,
                  false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? ha - 0.05f : -ha + 0.05f, d = (i & 2) ? d1 - 0.05f : d0 + 0.05f;
        kit_frame_box(kit, &f, MAT_IRON, a - 0.02f, a + 0.02f, 0.0f, y0, d - 0.02f, d + 0.02f,
                      false);
    }
    const vec3 tap[4] = {{TAP_A, 1.2f, 0.0f},
                         {TAP_A, 1.2f, TAP_D - 0.06f},
                         {TAP_A, 1.12f, TAP_D},
                         {TAP_A, TAP_Y, TAP_D}};
    kit_frame_pipe(kit, &f, MAT_STEEL, tap, 4, 0.012f, 8);
    for (int i = -1; i <= 1; i += 2)
        kit_frame_box(kit, &f, MAT_IRON, TAP_A + 0.08f * (float)i - 0.02f,
                      TAP_A + 0.08f * (float)i + 0.02f, 1.22f, 1.25f, 0.02f, 0.06f, false);
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, -ha, ha, 0.0f, y1, 0.0f, d1, true);
}

// Standing water on the slab round (x, z), an irregular pool about rx by rz.
static void puddle(Kit* kit, KitRng* rng, float x, float z, float rx, float rz) {
    enum { POOL = 14 };
    vec3 pool[POOL];
    for (int i = 0; i < POOL; i++) {
        const float t = 2.0f * GLM_PIf * (float)i / (float)POOL, r = kit_rrange(rng, 0.6f, 1.0f);
        glm_vec3_copy((vec3){x + r * rx * cosf(t), BASEMENT_Y + 0.002f, z + r * rz * sinf(t)},
                      pool[i]);
    }
    kit_polygon_facing(kit, MAT_CELLAR_WET, pool, POOL, (vec3){0.0f, 1.0f, 0.0f});
}

// A floor drain out from the tub, in the pool of water that never quite drains into it; and
// where else the damp stands: under the water heater, and along the west wall's foot.
static void drain(Kit* kit, KitRng* rng) {
    const float x = 3.75f, z = TUB_Z, y = BASEMENT_Y;
    puddle(kit, rng, x, z, 0.62f, 0.5f);
    puddle(kit, rng, HEATER_X + 0.25f, HEATER_Z + 0.35f, 0.5f, 0.35f);
    puddle(kit, rng, -4.55f, 15.6f, 0.3f, 0.9f);
    const vec2 grate[] = {{0.0f, 0.0f}, {0.09f, 0.0f}, {0.09f, 0.006f}, {0.0f, 0.006f}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_IRON, x, z, y, grate, KIT_COUNT(grate), 16);
    for (int k = -2; k <= 2; k++)
        kit_frame_box(kit, &KIT_WORLD, MAT_BLACK, x - 0.06f, x + 0.06f, y + 0.006f, y + 0.007f,
                      z + 0.025f * (float)k - 0.006f, z + 0.025f * (float)k + 0.006f, false);
}

/*
 * A unit of open shelving on the east wall from a0 along it, its boards bowed under what has been
 * put on them and left: paint cans on the bottom, jars of preserves nobody will eat on the two
 * middle boards, laid in thirds at the sag, and boxes on the top.
 */
static void shelf_unit(Kit* kit, KitRng* rng, float a0) {
    const KitFrame f = {{CELLAR_X1, BASEMENT_Y, 10.3f}, -0.5f * GLM_PIf};
    const float w = 1.2f, d = 0.4f, top = 1.95f, post = 0.04f;
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? a0 + w - post : a0, dd = (i & 2) ? d - post : 0.02f;
        kit_frame_box(kit, &f, MAT_WOOD, a, a + post, 0.0f, top, dd, dd + post, false);
    }
    const float boards[4] = {0.35f, 0.8f, 1.25f, 1.7f};
    for (int b = 0; b < 4; b++) {
        const float y = boards[b], sag = kit_rrange(rng, 0.008f, 0.022f);
        vec2 board[18];
        for (int i = 0; i <= 8; i++) {
            const float u = (float)i / 8.0f, at = y - sag * sinf(GLM_PIf * u);
            glm_vec2_copy((vec2){a0 + u * w, at}, board[i]);
            glm_vec2_copy((vec2){a0 + u * w, at - 0.022f}, board[17 - i]);
        }
        kit_frame_extrude(kit, &f, MAT_WOOD, board, 18, 0.0f, d);
        for (int third = 0; third < 3; third++) {
            const float s0 = a0 + 0.05f + (float)third * (w - 0.1f) / 3.0f;
            const float s1 = s0 + (w - 0.1f) / 3.0f;
            const float on = y - sag * sinf(GLM_PIf * (0.5f * (s0 + s1) - a0) / w);
            if (b == 1 || b == 2) {
                kitchen_preserves(kit, &f, rng, s0, s1, on);
            } else if (b == 0) {
                const vec2 can[] = {{0.0f, 0.0f},     {0.085f, 0.0f},  {0.085f, 0.17f},
                                    {0.078f, 0.185f}, {0.06f, 0.185f}, {0.0f, 0.18f}};
                if (kit_rnd(rng) < 0.8f)
                    kit_frame_lathe(kit, &f, MAT_ENAMEL, 0.5f * (s0 + s1),
                                    0.15f + 0.1f * kit_rnd(rng), on, can, KIT_COUNT(can), 14);
            } else if (b == 3 && kit_rnd(rng) < 0.7f) {
                const float h = kit_rrange(rng, 0.12f, 0.22f), dd = kit_rrange(rng, 0.25f, 0.36f);
                kit_frame_box(kit, &f, MAT_CARDBOARD, s0 + 0.01f, s1 - 0.01f, on, on + h, 0.02f, dd,
                              false);
            }
        }
    }
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, a0, a0 + w, 0.0f, top, 0.0f, d, true);
}

// Old cardboard boxes stacked against the front wall, taped shut once and never opened again;
// between the irradiance probes' columns, so none stands at the edge of one.
static void boxes(Kit* kit, KitRng* rng) {
    const float stacks[2] = {1.5f, 2.5f};
    for (int s = 0; s < 2; s++) {
        float y = BASEMENT_Y;
        const int n = kit_rnd(rng) < 0.5f ? 2 : 3;
        for (int k = 0; k < n; k++) {
            const float hw = kit_rrange(rng, 0.2f, 0.28f), hd = kit_rrange(rng, 0.18f, 0.25f);
            const float hh = kit_rrange(rng, 0.13f, 0.21f), yaw = kit_rrange(rng, -0.15f, 0.15f);
            const vec3 centre = {stacks[s] + kit_rrange(rng, -0.03f, 0.03f), y + hh,
                                 CELLAR_Z0 + 0.03f + hd};
            kit_box(kit, MAT_CARDBOARD, centre, (vec3){hw, hh, hd}, yaw, true);
            y += 2.0f * hh;
        }
    }
}

// A workbench on the east wall: a heavy top on four legs and a shelf under, a vise at its end, a
// pegboard over it with the tools that were put back, and on it a jar of screws and an oil can.
static void workbench(Kit* kit) {
    const KitFrame f = {{CELLAR_X1, BASEMENT_Y, 15.6f}, -0.5f * GLM_PIf};
    const float w = 1.8f, d = 0.6f, top = 0.9f, leg = 0.035f;
    kit_frame_box(kit, &f, MAT_WOOD, 0.0f, w, top - 0.05f, top, 0.0f, d, false);
    for (int i = 0; i < 4; i++) {
        const float a = (i & 1) ? w - 0.06f : 0.06f, dd = (i & 2) ? d - 0.06f : 0.06f;
        kit_frame_box(kit, &f, MAT_WOOD, a - leg, a + leg, 0.0f, top - 0.05f, dd - leg, dd + leg,
                      false);
    }
    kit_frame_box(kit, &f, MAT_WOOD, 0.05f, w - 0.05f, 0.18f, 0.2f, 0.05f, d - 0.05f, false);
    kit_frame_box(kit, &f, MAT_IRON, 0.12f, 0.32f, top, top + 0.09f, d - 0.07f, d + 0.02f, false);
    kit_frame_box(kit, &f, MAT_IRON, 0.12f, 0.32f, top + 0.02f, top + 0.09f, d + 0.02f, d + 0.05f,
                  false);
    const vec3 handle[2] = {{0.12f, top + 0.04f, d + 0.08f}, {0.32f, top + 0.04f, d + 0.08f}};
    kit_frame_pipe(kit, &f, MAT_STEEL, handle, 2, 0.008f, 6);

    kit_frame_box(kit, &f, MAT_CARDBOARD, 0.1f, w - 0.1f, top + 0.15f, top + 0.95f, 0.0f, 0.012f,
                  false);
    kit_frame_box(kit, &f, MAT_STEEL, 0.3f, 0.78f, top + 0.56f, top + 0.68f, 0.014f, 0.017f, false);
    kit_frame_box(kit, &f, MAT_WOOD, 0.78f, 0.9f, top + 0.53f, top + 0.7f, 0.012f, 0.04f, false);
    kit_frame_box(kit, &f, MAT_WOOD, 1.0f, 1.035f, top + 0.4f, top + 0.72f, 0.015f, 0.035f, false);
    kit_frame_box(kit, &f, MAT_IRON, 0.96f, 1.075f, top + 0.72f, top + 0.76f, 0.012f, 0.05f, false);
    for (int i = 0; i < 3; i++) {
        const float a = 1.25f + 0.1f * (float)i, len = 0.14f + 0.04f * (float)i;
        kit_frame_box(kit, &f, MAT_STEEL, a - 0.011f, a + 0.011f, top + 0.62f - len, top + 0.62f,
                      0.013f, 0.019f, false);
    }

    const vec2 jar[] = {
        {0.0f, 0.0f}, {0.04f, 0.0f}, {0.04f, 0.11f}, {0.032f, 0.12f}, {0.0f, 0.12f}};
    kit_frame_lathe(kit, &f, MAT_GLASS_CLEAR, 1.45f, 0.35f, top, jar, KIT_COUNT(jar), 14);
    const vec2 screws[] = {{0.0f, 0.004f}, {0.035f, 0.004f}, {0.035f, 0.06f}, {0.0f, 0.07f}};
    kit_frame_lathe(kit, &f, MAT_STEEL, 1.45f, 0.35f, top, screws, KIT_COUNT(screws), 10);
    const vec2 oil[] = {{0.0f, 0.0f}, {0.06f, 0.0f}, {0.06f, 0.06f}, {0.02f, 0.1f}, {0.0f, 0.1f}};
    kit_frame_lathe(kit, &f, MAT_ENAMEL, 1.65f, 0.3f, top, oil, KIT_COUNT(oil), 14);
    const vec3 spout[2] = {{1.65f, top + 0.09f, 0.3f}, {1.65f, top + 0.2f, 0.42f}};
    kit_frame_pipe(kit, &f, MAT_ENAMEL, spout, 2, 0.006f, 6);

    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, 0.0f, w, 0.0f, top, 0.0f, d, true);
}

/*
 * A small plank door in the front wall at head height, into the crawlspace under the porch:
 * hinged on the left, shut with a hasp and a padlock gone to rust, and nothing to say what is
 * kept in there.
 */
static void crawlspace_door(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = -1.15f, x1 = -0.55f, y0 = BASEMENT_Y + 1.55f, y1 = BASEMENT_Y + 2.2f;
    const float z = CELLAR_Z0, mid = 0.5f * (y0 + y1);
    kit_frame_box(kit, w, MAT_JOIST, x0 - 0.05f, x1 + 0.05f, y0 - 0.05f, y1 + 0.05f, z, z + 0.02f,
                  false);
    for (int i = 0; i < 4; i++) {
        const float a = x0 + 0.15f * (float)i;
        kit_frame_box(kit, w, MAT_WOOD, a + 0.003f, a + 0.147f, y0, y1, z + 0.02f, z + 0.045f,
                      false);
    }
    for (int i = 0; i < 2; i++) {
        const float y = i ? y1 - 0.16f : y0 + 0.08f;
        kit_frame_box(kit, w, MAT_WOOD, x0 + 0.03f, x1 - 0.03f, y, y + 0.08f, z + 0.045f,
                      z + 0.065f, false);
        kit_frame_box(kit, w, MAT_IRON, x0 - 0.04f, x0 + 0.2f, y + 0.02f, y + 0.06f, z + 0.065f,
                      z + 0.07f, false);
    }
    kit_frame_box(kit, w, MAT_IRON, x1 - 0.1f, x1 + 0.04f, mid - 0.022f, mid + 0.022f, z + 0.045f,
                  z + 0.052f, false);
    kit_frame_box(kit, w, MAT_IRON, x1 - 0.004f, x1 + 0.03f, mid - 0.1f, mid - 0.035f, z + 0.055f,
                  z + 0.075f, false);
    const vec3 shackle[4] = {{x1 + 0.0f, mid - 0.035f, z + 0.065f},
                             {x1 + 0.0f, mid - 0.005f, z + 0.065f},
                             {x1 + 0.026f, mid - 0.005f, z + 0.065f},
                             {x1 + 0.026f, mid - 0.035f, z + 0.065f}};
    kit_frame_pipe(kit, w, MAT_IRON, shackle, 4, 0.004f, 6);
}

void basement_build(Kit* kit, unsigned int seed) {
    foundation(kit);
    framing(kit);
    beam(kit);
    stair(kit);
    stairwell(kit);

    KitRng rng = {seed * 2654435761u + 4099u};
    furnace(kit);
    water_heater(kit);
    laundry_tub(kit);
    drain(kit, &rng);
    shelf_unit(kit, &rng, 0.0f);
    shelf_unit(kit, &rng, 1.3f);
    boxes(kit, &rng);
    workbench(kit);
    // A kitchen chair carried down and set facing the far corner, a little way from it.
    kitchen_chair(kit, &(KitFrame){{4.35f, BASEMENT_Y, 18.85f}, 0.25f * GLM_PIf});
    crawlspace_door(kit);

    // Earth under the porch, where the home's irradiance probes the basement brought below the
    // yard would otherwise hang in nothing outside the front wall. Closed, so the volume finds
    // them inside it and switches them off. Never seen.
    kit_frame_box(kit, &KIT_WORLD, MAT_DIRT, DIG_X0 - 0.5f, DIG_X1 + 0.5f, BASEMENT_Y - 0.5f, -0.4f,
                  DIG_Z0 - 2.0f, DIG_Z0, false);

    // The bulb's rose, which stays put while the bulb swings under it.
    const vec2 rose[] = {
        {0.0f, 0.0f}, {0.045f, 0.0f}, {0.04f, -0.018f}, {0.008f, -ROSE_DROP}, {0.0f, -ROSE_DROP}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_CERAMIC, BULB_X, BULB_Z, CEIL_Y, rose, KIT_COUNT(rose),
                    12);
}

// What swings, about the pivot at the origin: the cord, a black socket, the bulb's brass cap
// and its glass, and a pull chain hanging beside it with a bead on the end.
static void bulb_parts(Kit* kit) {
    const KitFrame* f = &KIT_WORLD;
    const vec3 cord[2] = {{0.0f, 0.0f, 0.0f}, {0.0f, -CORD_L, 0.0f}};
    kit_frame_pipe(kit, f, MAT_BLACK, cord, 2, 0.0035f, 6);
    const vec2 socket[] = {{0.0f, 0.0f},    {0.017f, 0.0f},   {0.019f, 0.01f}, {0.019f, 0.055f},
                           {0.012f, 0.07f}, {0.005f, 0.075f}, {0.0f, 0.075f}};
    kit_frame_lathe(kit, f, MAT_BLACK, 0.0f, 0.0f, -CORD_L - 0.075f, socket, KIT_COUNT(socket), 12);
    const vec2 cap[] = {{0.0f, 0.0f}, {0.0125f, 0.0f}, {0.0125f, 0.025f}, {0.0f, 0.025f}};
    kit_frame_lathe(kit, f, MAT_BRASS, 0.0f, 0.0f, -CORD_L - 0.1f, cap, KIT_COUNT(cap), 12);
    const vec2 glass[] = {{0.0f, 0.0f},          {0.014f, 0.004f}, {0.026f, 0.016f},
                          {0.03f, GLASS_WIDEST}, {0.028f, 0.05f},  {0.02f, 0.07f},
                          {0.014f, 0.082f},      {0.0125f, 0.09f}, {0.0f, 0.09f}};
    kit_frame_lathe(kit, f, MAT_BULB, 0.0f, 0.0f, -GLASS_FOOT, glass, KIT_COUNT(glass), 16);
    const vec3 chain[2] = {{0.021f, -CORD_L - 0.04f, 0.0f}, {0.021f, -CORD_L - 0.3f, 0.0f}};
    kit_frame_pipe(kit, f, MAT_STEEL, chain, 2, 0.0012f, 4);
    const vec2 bead[] = {{0.0f, 0.0f}, {0.005f, 0.004f}, {0.005f, 0.008f}, {0.0f, 0.012f}};
    kit_frame_lathe(kit, f, MAT_STEEL, 0.021f, 0.0f, -CORD_L - 0.312f, bead, KIT_COUNT(bead), 8);
}

// Where the cord hangs from, under the rose.
static const vec3 PIVOT = {BULB_X, CEIL_Y - ROSE_DROP, BULB_Z};

void basement_start(Basement* b, Engine* engine, Scene* scene, AudioSystem* audio,
                    unsigned int seed) {
    *b = (Basement){.drafted = -1.0,
                    .seed = seed,
                    .audio = audio,
                    .next_drip = DRIP_FIRST,
                    .drips = {seed * 2246822519u + 77u}};
    if (audio) {
        b->drip = audio_sound_from_file(audio, DRIP_PATH, AUDIO_BUS_SFX);
        if (!b->drip)
            fprintf(stderr, "silent: cannot load %s\n", DRIP_PATH);
    }
    // A kit of its own, so its node turns alone and its glass is its own to dim. It casts
    // nothing: swinging in its own light's kept views, it would be a mover in them every frame,
    // and it is its light's body besides.
    Kit kit;
    mats_kit(&kit, engine, scene);
    kit.casts_nothing = true;
    bulb_parts(&kit);
    b->glass = kit.materials[MAT_BULB];
    b->bulb = kit_finish(&kit, "basement_bulb");
    // A capture is kept for good, and the glass is somewhere else a moment later.
    b->bulb->capture_hidden = true;

    // Placed, dimmed and coloured each frame by bulb_update, before the first draws.
    const LightDesc desc = {.name = "basement_bulb",
                            .type = LIGHT_POINT,
                            .range = BULB_RANGE,
                            .cast_shadows = true,
                            .shadow_cache = true,
                            .shadow_near = 0.05f};
    b->light = create_light(&desc);
    scene_add_light(scene, b->light);
}

// Every few seconds a drop from the tap into the water under it, never quite on a beat.
static void drip(Basement* b, double time) {
    if (!b->drip || time < b->next_drip)
        return;
    b->next_drip = time + DRIP_EVERY + DRIP_SPREAD * kit_rnd(&b->drips);
    vec3 at = GLM_VEC3_ZERO_INIT;
    kit_frame_point(&TUB, TAP_A, TUB_WATER, TAP_D, at);
    AudioVoiceDesc d = {.volume = DRIP_VOLUME};
    glm_vec3_copy(at, d.position);
    audio_play_voice(b->audio, b->drip, &d);
}

static void bulb_update(Basement* b, const Door* door, double time) {
    if (!b->bulb || !b->light)
        return;
    if (b->drafted < 0.0 && door && door->travel >= DRAFT_OPEN)
        b->drafted = time;

    // A pendulum the cord's length long, pushed west by the draft and dying away, with a faint
    // sway of its own coming in under it: along the flight about z, across it about x.
    float along = 0.0f, across = 0.0f, reach = 0.0f;
    if (b->drafted >= 0.0) {
        const float u = (float)(time - b->drafted), t = (float)time;
        const float w = sqrtf(9.81f / BULB_DROP), fade = expf(-u / DRAFT_DECAY);
        const float sway = SWAY * glm_smoothstep(0.0f, 3.0f, u) * (0.65f + 0.35f * sinf(0.23f * t));
        along = -DRAFT_SWING * fade * sinf(w * u) + sway * sinf(w * t + 1.3f);
        across = -DRAFT_SIDE * fade * sinf(w * u + 0.4f) + 0.5f * sway * sinf(w * t + 2.9f);
        reach = DRAFT_SWING * fade * BULB_DROP;
    }
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    glm_translate_make(m, (float*)PIVOT);
    glm_rotate_z(m, along, m);
    glm_rotate_x(m, across, m);
    glm_mat4_copy(m, b->bulb->original_transform);
    vec3 at = GLM_VEC3_ZERO_INIT;
    glm_mat4_mulv3(m, (vec3){0.0f, -BULB_DROP, 0.0f}, 1.0f, at);
    light_set_position(b->light, at);
    b->light->shadow_follow = reach > FOLLOW_REACH;

    // The supply sags now and then, and the filament goes dim and orange with it; never out, or
    // the light would leave its cached shadow undrawn.
    const float level = lights_brownout(time, b->seed);
    vec3 colour = GLM_VEC3_ZERO_INIT;
    glm_vec3_lerp((float*)BULB_SAGGED, (float*)BULB_COLOUR, level, colour);
    b->light->intensity = BULB_CANDELA * level;
    glm_vec3_copy(colour, b->light->color);
    b->glass->emissive_strength = BULB_NITS * level;
    glm_vec3_copy(colour, b->glass->emissive);
}

void basement_update(Basement* b, const Door* door, double time) {
    bulb_update(b, door, time);
    drip(b, time);
}

// Filling the way in: its width, the partition's depth through, from the floor to the header.
static void bar(Basement* b, EntityManager* em, PhysicsWorld* physics) {
    if (b->bar || !em || !physics)
        return;
    b->bar = create_entity(em, "basement_bar");
    if (!b->bar)
        return;
    const KitOpening* o = &WAY_IN;
    glm_vec3_copy((vec3){0.5f * (o->from + o->to), 0.5f * (o->bottom + o->top), STAIRWELL_WALL_Z},
                  b->bar->position);
    PhysicsShapeDesc box = {.type = SHAPE_BOX,
                            .box.half_extents = {0.5f * (o->to - o->from),
                                                 0.5f * (o->top - o->bottom), 0.5f * INT_WALL},
                            .density = 0.0f};
    entity_add_rigid_body(b->bar, physics, &box, MOTION_STATIC, OBJ_LAYER_STATIC);
}

static void unbar(Basement* b, EntityManager* em) {
    if (!b->bar)
        return;
    destroy_entity(em, b->bar);
    b->bar = NULL;
}

bool basement_hold(Basement* b, EntityManager* em, PhysicsWorld* physics, const vec3 feet,
                   bool light) {
    if (light)
        unbar(b, em);
    else
        bar(b, em, physics);
    if (feet[0] < HALL_X0 && feet[2] > STAIRWELL_Z0 && feet[2] < CELLAR_Z1 &&
        feet[1] < BASEMENT_Y + FOOT_REACH) {
        const bool arrived = !b->at_foot;
        b->at_foot = true;
        return arrived && !light;
    }
    if (feet[1] > HALL_NEAR)
        b->at_foot = false;
    return false;
}
