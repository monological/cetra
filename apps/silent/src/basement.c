#include <math.h>

#include "cetra/light.h"

#include "basement.h"
#include "lights.h"
#include "mats.h"

#define SLAB_DEPTH  0.2f // the slab, under BASEMENT_Y
#define SILL_W      0.1f // the sill plate the framing sits on, along the walls' tops
#define JOIST_W     0.045f
#define JOIST_PITCH 0.4f
#define JOIST_LAP   0.1f // how far a joist runs on past the beam's middle, beside its neighbour
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

// The stairwell's paint: one coat over whatever its walls are built of, and the dark band along
// the flight over its nosings, level across the wall at its foot.
#define DADO_H (1.0f)
#define DADO_Y (BASEMENT_Y + DADO_H)
#define DADO_T 0.002f
#define COAT   0.006f
// How high the damp has climbed the basement's own walls.
#define DAMP_Y (BASEMENT_Y + 0.6f)
// The way from the stairwell's foot into the basement, on the right at the bottom: the whole
// width of the floor there, and a doorway's height under a header.
#define FOOT_HEAD (BASEMENT_Y + 2.1f)

// The bulb: on a cord from a rose on the stairwell's ceiling over the upper flight, low enough
// that the door opens on it at a standing eye's height. Lengths from the pivot under the rose.
#define BULB_X       (-2.75f)
#define BULB_Z       (0.5f * (STAIRWELL_Z0 + CELLAR_Z1))
#define ROSE_DROP    0.026f
#define CORD_L       0.82f
#define GLASS_FOOT   1.01f
#define BULB_DROP    (GLASS_FOOT - 0.034f) // to the glass's widest, where the light is
#define BULB_CANDELA 30.0f
#define BULB_RANGE   6.0f
#define BULB_NITS    1500.0f
// The draft's swing the first time the door opens: across the flight, pushed away from the
// door, a little to the side, dying away; and the faint sway it leaves.
#define DRAFT_SWING 0.16f // radians
#define DRAFT_SIDE  0.05f
#define DRAFT_DECAY 3.5f // seconds
#define SWAY        0.012f
// While the swing still carries the glass further than this, the bulb's shadow is drawn again
// each frame: a kept face shadows from where it was drawn.
#define REFRESH_REACH 0.015f

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

/*
 * The ground floor's framing, which is the basement's ceiling: sill plates on the walls, a rim
 * joist round them, the joists across from each wall to the beam, lapped past each other over
 * it, and the subfloor on them. Round the stairwell the joists it cuts hang from a header at the
 * landing's edge, and a doubled trimmer runs along its side under the partition. Only the faces
 * seen from below are drawn: the boards cover the rest.
 */
static void framing(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const unsigned below = KIT_FACES_ALL & ~KIT_FACE_UP;
    const float j0 = JOIST_Y0, j1 = SUBFLOOR_Y0, r = JOIST_W;
    const float trimmer = STAIRWELL_Z0 - 2.0f * r, header = CELLAR_HEAD_X + 2.0f * r;

    // The sill plates, the back one and the west one stopping at the stairwell, as the rims do.
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X1, 0.0f, j0, CELLAR_Z0,
                        CELLAR_Z0 + SILL_W, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_HEAD_X, CELLAR_X1, 0.0f, j0, CELLAR_Z1 - SILL_W,
                        CELLAR_Z1, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X0 + SILL_W, 0.0f, j0,
                        CELLAR_Z0 + SILL_W, trimmer, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X1 - SILL_W, CELLAR_X1, 0.0f, j0,
                        CELLAR_Z0 + SILL_W, CELLAR_Z1 - SILL_W, below);

    // The rims, the back one and the west one stopping at the stairwell.
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X1, j0, j1, CELLAR_Z0, CELLAR_Z0 + r,
                        below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_HEAD_X, CELLAR_X1, j0, j1, CELLAR_Z1 - r,
                        CELLAR_Z1, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X0 + r, j0, j1, CELLAR_Z0 + r, trimmer,
                        below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X1 - r, CELLAR_X1, j0, j1, CELLAR_Z0 + r,
                        CELLAR_Z1 - r, below);

    // The stairwell's trimmer and header.
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, JOIST_LAP, j0, j1, trimmer, STAIRWELL_Z0,
                        below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_HEAD_X, header, j0, j1, STAIRWELL_Z0,
                        CELLAR_Z1 - r, below);

    // The joists: the west ones at each pitch, the east ones a joist's width on, lapped.
    for (float z = CELLAR_Z0 + 0.5f * JOIST_PITCH; z + 1.5f * r < CELLAR_Z1 - r; z += JOIST_PITCH) {
        if (z + 0.5f * r < trimmer)
            kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0 + r, JOIST_LAP, j0, j1, z - 0.5f * r,
                                z + 0.5f * r, below);
        else if (z - 0.5f * r > STAIRWELL_Z0)
            kit_frame_box_faces(kit, w, MAT_JOIST, header, JOIST_LAP, j0, j1, z - 0.5f * r,
                                z + 0.5f * r, below);
        kit_frame_box_faces(kit, w, MAT_JOIST, -JOIST_LAP, CELLAR_X1 - r, j0, j1, z + 0.5f * r,
                            z + 1.5f * r, below);
    }

    // The subfloor, round the stairwell.
    const float sub1 = FLOOR_Y - FLOOR_BOARDS;
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X1, j1, sub1, CELLAR_Z0, STAIRWELL_Z0,
                        below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_HEAD_X, CELLAR_X1, j1, sub1, STAIRWELL_Z0,
                        CELLAR_Z1, below);
}

// A steel beam under the middle of the house, where the hall's east wall stands over it, on
// three lally columns.
static void beam(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float b = BEAM_HALF, top = JOIST_Y0;
    kit_frame_box(kit, w, MAT_ENAMEL, -b, b, top - FLANGE, top, CELLAR_Z0, CELLAR_Z1, false);
    kit_frame_box(kit, w, MAT_ENAMEL, -0.005f, 0.005f, BEAM_Y0 + FLANGE, top - FLANGE, CELLAR_Z0,
                  CELLAR_Z1, false);
    kit_frame_box(kit, w, MAT_ENAMEL, -b, b, BEAM_Y0, BEAM_Y0 + FLANGE, CELLAR_Z0, CELLAR_Z1,
                  false);
    for (int i = 0; i < KIT_COUNT(POSTS_Z); i++) {
        const float z = POSTS_Z[i], p = 0.08f;
        kit_prism(kit, MAT_ENAMEL, 0.0f, z, BASEMENT_Y + 0.012f, BEAM_Y0 - 0.012f, POST_R, 10,
                  true);
        kit_frame_box(kit, w, MAT_ENAMEL, -p, p, BEAM_Y0 - 0.012f, BEAM_Y0, z - p, z + p, false);
        kit_frame_box(kit, w, MAT_ENAMEL, -p, p, BASEMENT_Y, BASEMENT_Y + 0.012f, z - p, z + p,
                      false);
    }
}

// The height of the flight's pitch at x: the line through every tread's front edge.
static float nosing_y(float x) {
    return FLOOR_Y + (x - CELLAR_HEAD_X) * (CELLAR_RISE / CELLAR_GOING);
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
    const float foot = CELLAR_FOOT_X;
    const float heel =
        CELLAR_HEAD_X + (BASEMENT_Y + STRINGER_DOWN - FLOOR_Y) * (CELLAR_GOING / CELLAR_RISE);
    const vec2 stringer[5] = {{CELLAR_HEAD_X, nosing_y(CELLAR_HEAD_X) + STRINGER_UP},
                              {foot, nosing_y(foot) + STRINGER_UP},
                              {foot, BASEMENT_Y},
                              {heel, BASEMENT_Y},
                              {CELLAR_HEAD_X, nosing_y(CELLAR_HEAD_X) - STRINGER_DOWN}};
    kit_frame_extrude(kit, w, MAT_JOIST, stringer, 5, STAIRWELL_Z0, z0);
    kit_frame_extrude(kit, w, MAT_JOIST, stringer, 5, z1, CELLAR_Z1);

    // A handrail on the back wall, on iron brackets.
    const float rz = CELLAR_Z1 - 0.06f;
    raked_rail(kit, CELLAR_HEAD_X - 0.15f, foot - 0.05f, rz - RAIL_T, rz);
    const float brackets[3] = {-2.2f, -3.0f, -3.8f};
    for (int i = 0; i < 3; i++) {
        const float x = brackets[i], under = nosing_y(x) + RAIL_H - RAIL_D;
        kit_frame_box(kit, w, MAT_IRON, x - 0.012f, x + 0.012f, under - 0.03f, under,
                      rz - 0.5f * RAIL_T, CELLAR_Z1, false);
    }
}

// A coat of `mat` over a wall's face, cut round its openings: `w` an axis-aligned wall COAT thick
// standing at `at`, as kit_wall takes one.
static void coat(Kit* kit, int mat, KitWall w) {
    const KitWallFrame wf = kit_wall_frame(&w);
    w.at = wf.at;
    w.thick = COAT;
    kit_frame_panel(kit, &wf.f, mat, &w);
}

// The dark band over a coat on frame `f`: the outline in (a, y), just proud of the coat's face at
// d, toward `out`.
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
 * the floor and the concrete below alike, and the dark band runs down the two long walls along
 * the flight, DADO_H over its nosings, and level across the wall at its foot.
 */
static void stairwell(Kit* kit) {
    const float s0 = STAIRWELL_WALL_Z - 0.5f * INT_WALL, hall = HALL_X0 - 0.5f * INT_WALL;
    const float foot = CELLAR_FOOT_X;
    cellar_wall(kit, foot, HALL_X0, BASEMENT_Y, JOIST_Y0, s0, STAIRWELL_Z0);
    cellar_wall(kit, CELLAR_HEAD_X, HALL_X0, BASEMENT_Y, JOIST_Y0, STAIRWELL_Z0, CELLAR_Z1);
    kit_frame_box(kit, &KIT_WORLD, MAT_CELLAR_CONCRETE, CELLAR_X0, foot, FOOT_HEAD, JOIST_Y0, s0,
                  STAIRWELL_Z0, true);

    coat(kit, MAT_CELLAR_WALL,
         (KitWall){.along_x = true,
                   .at = CELLAR_Z1 - 0.5f * COAT,
                   .from = CELLAR_X0,
                   .to = hall,
                   .y0 = BASEMENT_Y,
                   .y1 = CEIL_Y,
                   .openings = {{STAIR_WIN_X0, STAIR_WIN_X1, STAIR_WIN_SILL, STAIR_WIN_HEAD}},
                   .opening_count = 1});
    coat(kit, MAT_CELLAR_WALL,
         (KitWall){.along_x = true,
                   .at = STAIRWELL_Z0 + 0.5f * COAT,
                   .from = CELLAR_X0,
                   .to = hall,
                   .y0 = BASEMENT_Y,
                   .y1 = CEIL_Y,
                   .openings = {{CELLAR_X0, foot, BASEMENT_Y, FOOT_HEAD, .door = true}},
                   .opening_count = 1});
    coat(kit, MAT_CELLAR_WALL,
         (KitWall){.at = CELLAR_X0 + 0.5f * COAT,
                   .from = STAIRWELL_Z0,
                   .to = CELLAR_Z1,
                   .y0 = BASEMENT_Y,
                   .y1 = CEIL_Y});
    coat(kit, MAT_CELLAR_WALL,
         (KitWall){
             .at = hall - 0.5f * COAT,
             .from = STAIRWELL_Z0,
             .to = CELLAR_Z1,
             .y0 = FLOOR_Y,
             .y1 = CEIL_Y,
             .openings = {{BASEMENT_DOOR_Z0, BASEMENT_DOOR_Z1, FLOOR_Y, DOOR_HEAD, .door = true}},
             .opening_count = 1});
    coat(kit, MAT_CELLAR_DADO,
         (KitWall){.at = CELLAR_HEAD_X - 0.5f * COAT,
                   .from = STAIRWELL_Z0,
                   .to = CELLAR_Z1,
                   .y0 = BASEMENT_Y,
                   .y1 = FLOOR_Y});

    // The band: down the flight's rake to where the nosings would meet the floor, and level on.
    const float level = CELLAR_HEAD_X + (BASEMENT_Y - FLOOR_Y) * (CELLAR_GOING / CELLAR_RISE);
    const vec2 north[5] = {{hall, nosing_y(hall) + DADO_H},
                           {level, DADO_Y},
                           {CELLAR_X0, DADO_Y},
                           {CELLAR_X0, BASEMENT_Y},
                           {hall, BASEMENT_Y}};
    dado(kit, &KIT_WORLD, north, 5, CELLAR_Z1 - COAT, -1.0f);
    const vec2 south[4] = {{hall, nosing_y(hall) + DADO_H},
                           {foot, nosing_y(foot) + DADO_H},
                           {foot, BASEMENT_Y},
                           {hall, BASEMENT_Y}};
    dado(kit, &KIT_WORLD, south, 4, STAIRWELL_Z0 + COAT, 1.0f);
    const vec2 west[4] = {{STAIRWELL_Z0, BASEMENT_Y},
                          {CELLAR_Z1, BASEMENT_Y},
                          {CELLAR_Z1, DADO_Y},
                          {STAIRWELL_Z0, DADO_Y}};
    dado(kit, &KIT_WORLD_Z, west, 4, -(CELLAR_X0 + COAT), -1.0f);
}

void basement_build(Kit* kit) {
    foundation(kit);
    framing(kit);
    beam(kit);
    stair(kit);
    stairwell(kit);

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
    const vec2 glass[] = {{0.0f, 0.0f},     {0.014f, 0.004f}, {0.026f, 0.016f},
                          {0.03f, 0.034f},  {0.028f, 0.05f},  {0.02f, 0.07f},
                          {0.014f, 0.082f}, {0.0125f, 0.09f}, {0.0f, 0.09f}};
    kit_frame_lathe(kit, f, MAT_BULB, 0.0f, 0.0f, -GLASS_FOOT, glass, KIT_COUNT(glass), 16);
    const vec3 chain[2] = {{0.021f, -CORD_L - 0.04f, 0.0f}, {0.021f, -CORD_L - 0.3f, 0.0f}};
    kit_frame_pipe(kit, f, MAT_STEEL, chain, 2, 0.0012f, 4);
    const vec2 bead[] = {{0.0f, 0.0f}, {0.005f, 0.004f}, {0.005f, 0.008f}, {0.0f, 0.012f}};
    kit_frame_lathe(kit, f, MAT_STEEL, 0.021f, 0.0f, -CORD_L - 0.312f, bead, KIT_COUNT(bead), 8);
}

static void pivot(vec3 out) {
    glm_vec3_copy((vec3){BULB_X, CEIL_Y - ROSE_DROP, BULB_Z}, out);
}

void basement_start(Basement* b, Engine* engine, Scene* scene, unsigned int seed) {
    *b = (Basement){.drafted = -1.0, .seed = seed};
    // A kit of its own, so its node turns alone and its glass is its own to dim. It casts
    // nothing: swinging in its own light's kept views, it would be a mover in them every frame,
    // and it is its light's body besides.
    Kit kit;
    kit_init(&kit, scene, NULL, NULL);
    mats_register(&kit, engine, scene);
    kit.casts_nothing = true;
    bulb_parts(&kit);
    b->glass = kit.materials[MAT_BULB];
    b->bulb = kit_finish(&kit, "basement_bulb");
    // A capture is kept for good, and the glass is somewhere else a moment later.
    b->bulb->capture_hidden = true;

    vec3 at = GLM_VEC3_ZERO_INIT;
    pivot(at);
    at[1] -= BULB_DROP;
    const LightDesc desc = {.name = "basement_bulb",
                            .type = LIGHT_POINT,
                            .position = {at[0], at[1], at[2]},
                            .color = {1.0f, 0.78f, 0.52f},
                            .intensity = BULB_CANDELA,
                            .range = BULB_RANGE,
                            .cast_shadows = true,
                            .shadow_cache = true,
                            .shadow_near = 0.05f};
    b->light = create_light(&desc);
    scene_add_light(scene, b->light);
}

void basement_update(Basement* b, const Door* door, double time) {
    if (!b->bulb || !b->light)
        return;
    if (b->drafted < 0.0 && door && door->travel >= 0.25f)
        b->drafted = time;

    // A pendulum the cord's length long, pushed west by the draft and dying away, with a faint
    // sway of its own coming in under it.
    float across = 0.0f, side = 0.0f, reach = 0.0f;
    if (b->drafted >= 0.0) {
        const float u = (float)(time - b->drafted), t = (float)time;
        const float w = sqrtf(9.81f / BULB_DROP), fade = expf(-u / DRAFT_DECAY);
        const float sway = SWAY * glm_smoothstep(0.0f, 3.0f, u) * (0.65f + 0.35f * sinf(0.23f * t));
        across = -DRAFT_SWING * fade * sinf(w * u) + sway * sinf(w * t + 1.3f);
        side = -DRAFT_SIDE * fade * sinf(w * u + 0.4f) + 0.5f * sway * sinf(w * t + 2.9f);
        reach = DRAFT_SWING * fade * BULB_DROP;
    }
    vec3 p = GLM_VEC3_ZERO_INIT;
    pivot(p);
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    glm_translate_make(m, p);
    glm_rotate_z(m, across, m);
    glm_rotate_x(m, side, m);
    glm_mat4_copy(m, b->bulb->original_transform);
    vec3 at = GLM_VEC3_ZERO_INIT;
    glm_mat4_mulv3(m, (vec3){0.0f, -BULB_DROP, 0.0f}, 1.0f, at);
    light_set_position(b->light, at);
    b->light->shadow_refresh = reach > REFRESH_REACH;

    // The supply sags now and then, and the filament goes dim and orange with it; never out, or
    // the light would leave its cached shadow undrawn.
    const float level = lights_brownout(time, b->seed);
    vec3 colour = GLM_VEC3_ZERO_INIT;
    glm_vec3_lerp((vec3){1.0f, 0.55f, 0.25f}, (vec3){1.0f, 0.78f, 0.52f}, level, colour);
    b->light->intensity = BULB_CANDELA * level;
    glm_vec3_copy(colour, b->light->color);
    b->glass->emissive_strength = BULB_NITS * level;
    glm_vec3_copy(colour, b->glass->emissive);
}
