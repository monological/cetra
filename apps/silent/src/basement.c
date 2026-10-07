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

// The posts under the beam, between the irradiance probes' rows at every half metre of z.
static const float POSTS_Z[] = {12.0f, 15.0f, 18.0f};

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

void basement_build(Kit* kit) {
    foundation(kit);
    framing(kit);
    beam(kit);

    // Earth under the porch, where the home's irradiance probes the basement brought below the
    // yard would otherwise hang in nothing outside the front wall. Closed, so the volume finds
    // them inside it and switches them off. Never seen.
    kit_frame_box(kit, &KIT_WORLD, MAT_DIRT, DIG_X0 - 0.5f, DIG_X1 + 0.5f, BASEMENT_Y - 0.1f, -0.4f,
                  DIG_Z0 - 2.0f, DIG_Z0, false);
}
