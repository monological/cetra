#include "basement.h"
#include "mats.h"

#define SLAB_DEPTH  0.15f    // the slab, under BASEMENT_Y
#define TIDE_Y      (-1.45f) // how high up the walls the damp has climbed
#define SILL_W      0.1f     // the sill plate the framing sits on, along the walls' tops
#define JOIST_W     0.045f
#define JOIST_PITCH 0.4f
#define JOIST_LAP   0.1f // how far a joist runs on past the beam's middle, beside its neighbour
#define BEAM_HALF   0.065f
#define BEAM_Y0     (-0.18f)
#define FLANGE      0.015f
#define POST_R      0.045f

// The flight down: steep, as a cellar's is, and every riser under the player's step.
#define CELLAR_RISERS 11
#define CELLAR_RISE   ((FLOOR_Y - BASEMENT_Y) / (float)CELLAR_RISERS)
#define CELLAR_GOING  0.23f
#define TREAD_T       0.035f
#define NOSING        0.02f
#define STRINGER_W    0.05f
#define STRINGER_UP 0.015f  // its top edge over the line of the nosings: under the open door's leaf
#define STRINGER_DOWN 0.26f // and its foot under it
#define RAIL_H        0.86f // a handrail's top over the nosings
#define RAIL_T        0.035f
#define RAIL_D        0.07f

// The posts under the beam, between the irradiance probes' rows at every half metre of z.
static const float POSTS_Z[] = {12.0f, 15.0f, 18.0f};
// And the two under the stair's open side, which the rail runs between. The lower stands a
// tread short of the foot, so the way round into the basement there is a body's width.
static const float STAIR_POSTS_X[] = {-3.30f, -4.03f};

/*
 * The foundation from the slab's foot to the sill: damp to the tide line and dirty rubble over
 * it, the house's outside walls standing on its top. And the slab.
 */
static void foundation(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float foot = BASEMENT_Y - SLAB_DEPTH;
    const struct {
        int mat;
        float y0, y1;
    } BANDS[] = {{MAT_CELLAR_DAMP, foot, TIDE_Y}, {MAT_CELLAR_STONE, TIDE_Y, 0.0f}};
    for (int b = 0; b < KIT_COUNT(BANDS); b++) {
        const int m = BANDS[b].mat;
        const float y0 = BANDS[b].y0, y1 = BANDS[b].y1;
        kit_frame_box(kit, w, m, DIG_X0, DIG_X1, y0, y1, DIG_Z0, CELLAR_Z0, true);
        kit_frame_box(kit, w, m, DIG_X0, DIG_X1, y0, y1, CELLAR_Z1, DIG_Z1, true);
        kit_frame_box(kit, w, m, DIG_X0, CELLAR_X0, y0, y1, CELLAR_Z0, CELLAR_Z1, true);
        kit_frame_box(kit, w, m, CELLAR_X1, DIG_X1, y0, y1, CELLAR_Z0, CELLAR_Z1, true);
    }
    kit_frame_box(kit, w, MAT_CELLAR_FLOOR, CELLAR_X0, CELLAR_X1, foot, BASEMENT_Y, CELLAR_Z0,
                  CELLAR_Z1, true);
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

    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X1, 0.0f, j0, CELLAR_Z0,
                        CELLAR_Z0 + SILL_W, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X1, 0.0f, j0, CELLAR_Z1 - SILL_W,
                        CELLAR_Z1, below);
    kit_frame_box_faces(kit, w, MAT_JOIST, CELLAR_X0, CELLAR_X0 + SILL_W, 0.0f, j0,
                        CELLAR_Z0 + SILL_W, CELLAR_Z1 - SILL_W, below);
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
 * The stair down: a steep cellar flight going west from the landing inside the door, open
 * treads housed between two stringers, the last riser landing short of the west wall, where the
 * basement opens to the south under the floor. Each tread's body is one rise deep, so what is
 * under the flight is not filled in. Nothing stops a step off the open side but the rail's
 * posts: above the last tread a body standing on the flight still reaches the floor overhead,
 * whose own bodies keep it there.
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
    const float foot = CELLAR_HEAD_X - (float)(CELLAR_RISERS - 1) * CELLAR_GOING;
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
    const float brackets[3] = {-2.5f, -3.4f, -4.2f};
    for (int i = 0; i < 3; i++) {
        const float x = brackets[i], under = nosing_y(x) + RAIL_H - RAIL_D;
        kit_frame_box(kit, w, MAT_IRON, x - 0.012f, x + 0.012f, under - 0.03f, under,
                      rz - 0.5f * RAIL_T, CELLAR_Z1, false);
    }

    // The open side, under the floor: two posts from the slab to the trimmer, and a rail between.
    const float pz0 = STAIRWELL_Z0 - 0.08f, p = 0.04f;
    for (int i = 0; i < KIT_COUNT(STAIR_POSTS_X); i++) {
        const float x = STAIR_POSTS_X[i];
        kit_frame_box(kit, w, MAT_JOIST, x - p, x + p, BASEMENT_Y, JOIST_Y0, pz0, STAIRWELL_Z0,
                      true);
    }
    raked_rail(kit, STAIR_POSTS_X[0], STAIR_POSTS_X[1], STAIRWELL_Z0, STAIRWELL_Z0 + RAIL_T);
}

void basement_build(Kit* kit) {
    foundation(kit);
    framing(kit);
    beam(kit);
    stair(kit);

    // Earth under the porch, where the home's irradiance probes the basement brought below the
    // yard would otherwise hang in nothing outside the front wall. Closed, so the volume finds
    // them inside it and switches them off. Never seen.
    kit_frame_box(kit, &KIT_WORLD, MAT_DIRT, DIG_X0 - 0.5f, DIG_X1 + 0.5f, BASEMENT_Y - 0.1f, -0.4f,
                  DIG_Z0 - 2.0f, DIG_Z0, false);
}
