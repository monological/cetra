#include <math.h>

#include "house.h"
#include "houses.h"
#include "layout.h"
#include "mats.h"
#include "ornament.h"
#include "tower.h"

// Half an exterior wall: how far the walls along X reach past the corners so they close.
#define CORNER (0.5f * EXT_WALL)

/*
 * The main roof: one ridge from front to back over the middle, at a Gothic pitch, so the
 * street sees a tall gable. Its slopes fall MAIN_PITCH a metre to eaves overhanging the side
 * walls by EAVE_OVERHANG, and it runs RAKE_OVERHANG past the gables.
 */
#define MAIN_PITCH    1.19f // tan 50 degrees
#define EAVE_OVERHANG 0.45f
#define RAKE_OVERHANG 0.4f

// The front and west walls stop where they meet the tower's faces.
#define FRONT_FROM (TOWER_X + TOWER_APOTHEM)
#define WEST_FROM  (TOWER_Z + TOWER_APOTHEM)

// The gutter on the pent strip, its downpipe, and the leaks as drops a second at the reference
// rain: a joint seeps, the lip over the kitchen window overflows where the gutter sags, and the
// shoe at the pipe's foot runs nearly in a stream.
#define DOWNPIPE_R     0.035f
#define GUTTER_SHEET   0.015f
#define JOINT_DRIPS    3.0f
#define OVERFLOW_DRIPS 40.0f
#define SPOUT_DRIPS    40.0f

float house_roof_y(float x) {
    return EAVE_Y + (HOUSE_X1 + CORNER - fabsf(x)) * MAIN_PITCH;
}

float house_roof_under_y(float x) {
    return house_roof_y(x) - ROOF_THICK;
}

/*
 * A roof slab under the plane y = y0 + gx x + gz z, over the outline `xz` in plan: slate on
 * top, and a roof's thickness under it boarding -- the soffit outside, and over the great hall
 * the ceiling it is open to -- with a fascia down every edge.
 */
static void roof_slab(Kit* kit, const vec2* xz, int count, float y0, float gx, float gz,
                      float thick) {
    vec3 top[KIT_MAX_OUTLINE] = {{0.0f}};
    for (int i = 0; i < count && i < KIT_MAX_OUTLINE; i++)
        glm_vec3_copy((vec3){xz[i][0], y0 + gx * xz[i][0] + gz * xz[i][1], xz[i][1]}, top[i]);
    kit_extrude(kit, MAT_SLATE, MAT_MAHOGANY, MAT_SIDING_DARK, top, count,
                (vec3){0.0f, -thick, 0.0f});
}

/*
 * The main roof's two slopes, the west one cut round the tower, which stands up through it;
 * the gables under them, the front one cut back to the tower's face; and the wall closing the
 * attic over the front rooms from the great hall, which is open to the roof.
 */
static void main_roof(Kit* kit) {
    const float x_eave = HOUSE_X1 + CORNER + EAVE_OVERHANG;
    const float z0 = HOUSE_FRONT_Z - CORNER - RAKE_OVERHANG;
    const float z1 = HOUSE_BACK_Z + CORNER + RAKE_OVERHANG;
    const float ridge = house_roof_y(0.0f);
    vec2 west[KIT_MAX_OUTLINE];
    const int n = tower_notch(-x_eave, z0, 0.0f, z1, TOWER_OUTER, west);
    roof_slab(kit, west, n, ridge, MAIN_PITCH, 0.0f, ROOF_THICK);
    const vec2 east[4] = {{0.0f, z0}, {x_eave, z0}, {x_eave, z1}, {0.0f, z1}};
    roof_slab(kit, east, 4, ridge, -MAIN_PITCH, 0.0f, ROOF_THICK);

    const vec2 inner[5] = {{HOUSE_X0, EAVE_Y},
                           {HOUSE_X1, EAVE_Y},
                           {HOUSE_X1, house_roof_y(HOUSE_X1)},
                           {0.0f, ridge},
                           {HOUSE_X0, house_roof_y(HOUSE_X0)}};
    kit_frame_extrude(kit, &KIT_WORLD, MAT_PLASTER, inner, 5, KITCHEN_BACK_Z - 0.5f * INT_WALL,
                      KITCHEN_BACK_Z + 0.5f * INT_WALL);

    /*
     * The gables, each a wall up to the roof from the eave and dressed: boards up to the rakes,
     * a cusped bargeboard down each one at the roof's edge, and a finial at its apex with its
     * pendant. The front one stops at the tower's face, its boards and its west rake with it.
     */
    const float side = HOUSE_X1 + CORNER, tower_face = FRONT_FROM + CORNER;
    const struct {
        HouseWall wall;
        float z, edge;  // the wall's middle, and the roof's edge over it
        float from;     // where the wall starts along x, the west end
        float boards;   // where its boards start
        float rake_end; // where its west bargeboard ends
    } gables[2] = {{HOUSE_WALL_FRONT, HOUSE_FRONT_Z, z0, FRONT_FROM, tower_face, tower_face},
                   {HOUSE_WALL_BACK, HOUSE_BACK_Z, z1, -side, -side, -x_eave}};
    for (int g = 0; g < 2; g++) {
        const float from = gables[g].from, d = gables[g].edge;
        // A back gable starting at the corner has no fourth corner: the rake starts there.
        const vec2 wall[4] = {
            {from, EAVE_Y}, {side, EAVE_Y}, {0.0f, ridge}, {from, house_roof_y(from)}};
        kit_frame_extrude(kit, &KIT_WORLD, MAT_SIDING_DARK, wall, from > -side ? 4 : 3,
                          gables[g].z - CORNER, gables[g].z + CORNER);
        const Facade s = facade_of(house_wall(gables[g].wall));
        ornament_gable_battens(kit, &s, gables[g].boards + 0.1f, side - 0.1f, EAVE_Y + 0.04f, 0.0f,
                               ridge, MAIN_PITCH);
        const vec2 apex = {0.0f, ridge + 0.03f};
        const vec2 right = {x_eave, house_roof_y(x_eave) + 0.03f};
        const vec2 left = {gables[g].rake_end, house_roof_y(gables[g].rake_end) + 0.03f};
        ornament_bargeboard(kit, &KIT_WORLD, right, apex, d - 0.02f, d + 0.02f);
        ornament_bargeboard(kit, &KIT_WORLD, left, apex, d - 0.02f, d + 0.02f);
        ornament_finial(kit, &KIT_WORLD, MAT_SIDING_DARK, 0.0f, ridge + 0.03f, d, 1.2f, 0.7f);
    }
    const KitFrame ridge_line = KIT_WORLD_Z;
    ornament_cresting(kit, &ridge_line, z0 + 0.3f, z1 - 0.3f, ridge, 0.0f);

    // The bare side eaves drip, the west one from past the tower.
    const float drip_y = house_roof_under_y(x_eave);
    const float west_from = TOWER_Z + TOWER_OUTER + 0.1f;
    kit_drip_run(kit, &KIT_WORLD, (vec3){-x_eave, drip_y, west_from}, (vec3){-x_eave, drip_y, z1},
                 EAVE_DRIPS_PER_M, 0.0f);
    kit_drip_run(kit, &KIT_WORLD, (vec3){x_eave, drip_y, z0}, (vec3){x_eave, drip_y, z1},
                 EAVE_DRIPS_PER_M, 0.0f);
}

/*
 * A channel open at the top, and a downpipe at the east end that drops to the wall and down it
 * to a shoe kicked out over the yard. It leaks at a joint by the porch, over the kitchen window
 * where it sags, and from the shoe.
 */
void house_front_gutter(Kit* kit, float x0, float x1, float edge_y, float fascia) {
    const KitFrame* w = &KIT_WORLD;
    const float bottom = edge_y - 0.11f, lip = edge_y, lip_z = fascia - 0.13f;
    kit_frame_box(kit, w, MAT_TRIM, x0, x1, bottom, bottom + GUTTER_SHEET, lip_z, fascia, false);
    kit_frame_box(kit, w, MAT_TRIM, x0, x1, bottom, lip, lip_z, lip_z + GUTTER_SHEET, false);
    const float ends[] = {x0, x1};
    for (int e = 0; e < 2; e++)
        kit_frame_box(kit, w, MAT_TRIM, ends[e] - 0.5f * GUTTER_SHEET,
                      ends[e] + 0.5f * GUTTER_SHEET, bottom, lip, lip_z, fascia, false);

    // Out of the gutter's floor, back to the wall in a swan neck, and down it, held a
    // bracket's 15 mm off the siding.
    const float pipe_x = HOUSE_X1 - 0.25f;
    const float mid = 0.5f * (lip_z + fascia);
    const float wall = HOUSE_FRONT_Z - CORNER - DOWNPIPE_R - 0.015f;
    const vec3 path[] = {{pipe_x, bottom, mid},
                         {pipe_x, bottom - 0.12f, mid},
                         {pipe_x, bottom - 0.24f, wall},
                         {pipe_x, 0.3f, wall},
                         {pipe_x, 0.14f, wall - 0.14f}};
    const int n = (int)(sizeof(path) / sizeof(path[0]));
    kit_frame_pipe(kit, w, MAT_TRIM, path, n, DOWNPIPE_R, 10);

    const vec3 joint = {x0 + 0.3f, bottom, mid};
    kit_drip(kit, w, joint, joint, JOINT_DRIPS, 0.0f);
    const float window = 0.5f * (KITCHEN_WIN_X0 + KITCHEN_WIN_X1);
    kit_drip(kit, w, (vec3){window - 0.15f, lip, lip_z - 0.005f},
             (vec3){window + 0.15f, lip, lip_z - 0.005f}, OVERFLOW_DRIPS, 0.0f);
    // Just past the shoe's mouth.
    const vec3 spout = {pipe_x, path[n - 1][1] - 0.02f, path[n - 1][2] - 0.02f};
    kit_drip(kit, w, spout, spout, SPOUT_DRIPS, 0.0f);
}

/*
 * The skirt roof: a deep slope over the porch, from the tower's face to just past the door,
 * on posts, and a strip past it to the east corner with the gutter. Its front edge drips.
 */
static void pent_roof(Kit* kit) {
    const float wall = HOUSE_FRONT_Z - CORNER;
    const float y0 = PENT_Y - PENT_PITCH * wall; // the plane's height at z = 0
    const float tower_face = FRONT_FROM + CORNER;
    const float porch_edge = PORCH_Z0 - 0.1f, strip_edge = wall - PENT_DEPTH;
    const vec2 porch[4] = {{tower_face, porch_edge},
                           {PORCH_ROOF_X1, porch_edge},
                           {PORCH_ROOF_X1, wall},
                           {tower_face, wall}};
    roof_slab(kit, porch, 4, y0, 0.0f, PENT_PITCH, PENT_THICK);
    const float x_end = HOUSE_X1 + CORNER + 0.3f;
    const vec2 strip[4] = {
        {PORCH_ROOF_X1, strip_edge}, {x_end, strip_edge}, {x_end, wall}, {PORCH_ROOF_X1, wall}};
    roof_slab(kit, strip, 4, y0, 0.0f, PENT_PITCH, PENT_THICK);

    // The porch roof on two turned posts at its front corners, with an arched board across the
    // front between them -- a wide Tudor arch -- and a pointed one down each side to the wall,
    // and a balustrade either side of the steps.
    const float porch_under = y0 + PENT_PITCH * porch_edge - PENT_THICK;
    const float post_z = PORCH_Z0 + 0.1f, post_top = y0 + PENT_PITCH * post_z - PENT_THICK;
    const float posts[2] = {PORCH_X0 + 0.1f, PORCH_X1 - 0.1f};
    for (int i = 0; i < 2; i++)
        ornament_post(kit, &KIT_WORLD, MAT_SIDING_DARK, posts[i], post_z, FLOOR_Y, post_top, 0.06f);
    const float spring = post_top - 0.5f;
    ornament_arch_board(kit, &KIT_WORLD, MAT_SIDING_DARK, posts[0] + 0.07f, posts[1] - 0.07f,
                        spring, post_top, KIT_ARCH_TUDOR, 0.36f, post_z);
    const KitFrame side = KIT_WORLD_Z;
    for (int i = 0; i < 2; i++)
        ornament_arch_board(kit, &side, MAT_SIDING_DARK, post_z + 0.07f, wall, spring, post_top,
                            KIT_ARCH_POINTED, 0.36f, -posts[i]);
    // Open where the path comes up to the steps.
    ornament_balustrade(kit, &KIT_WORLD, MAT_SIDING_DARK, posts[0] + 0.08f, PATH_X0 + 0.05f,
                        FLOOR_Y, post_z);
    ornament_balustrade(kit, &KIT_WORLD, MAT_SIDING_DARK, PATH_X1, posts[1] - 0.08f, FLOOR_Y,
                        post_z);
    for (int i = 0; i < 2; i++)
        ornament_balustrade(kit, &side, MAT_SIDING_DARK, post_z + 0.08f, wall - 0.02f, FLOOR_Y,
                            -posts[i]);
    kit_drip_run(kit, &KIT_WORLD, (vec3){tower_face + 0.1f, porch_under, porch_edge},
                 (vec3){PORCH_ROOF_X1 - 0.05f, porch_under, porch_edge}, PORCH_DRIPS_PER_M, 0.0f);
    house_front_gutter(kit, PORCH_ROOF_X1 + 0.05f, x_end - 0.05f,
                       y0 + PENT_PITCH * strip_edge - PENT_THICK, strip_edge);
}

// A pane of `glass` in opening `i` of the axis-aligned wall `w`.
static void pane_in(Kit* kit, const KitWall* w, int i, int glass) {
    const KitWallFrame wf = kit_wall_frame(w);
    kit_frame_pane(kit, &wf.f, glass, &w->openings[i], wf.at, w->thick);
}

/*
 * A shut door filling opening `i` of the axis-aligned wall `w`: door.c's Gothic leaf, built
 * in the house's kit since it never moves, and a body through the wall. Its strapped face is
 * toward the side it is seen from: the wall's inner side when `inner_face`, else its outer.
 */
static void door_in(Kit* kit, const KitWall* w, int i, bool inner_face) {
    const KitWallFrame wf = kit_wall_frame(w);
    const KitOpening* o = &w->openings[i];
    // The leaf's straps are on its own -d face; turn it half round when they would face the
    // wrong way, which runs its a the other way too.
    const bool turn = (inner_face ? wf.inner : -wf.inner) > 0;
    KitFrame leaf = {{0.0f, 0.0f, 0.0f}, wf.f.yaw + (turn ? GLM_PIf : 0.0f)};
    kit_frame_point(&wf.f, 0.0f, 0.0f, wf.at, leaf.origin);
    KitOpening shape = *o;
    if (turn) {
        shape.from = -o->to;
        shape.to = -o->from;
    }
    door_leaf(kit, &leaf, &shape, DOOR_THICK);
    kit_frame_plug(kit, &wf.f, o, wf.at, w->thick);
}

// A lancet from a0 to a1 along a wall, its sill at `sill`, springing at `spring` and rising
// `rise` to its point.
#define LANCET(a0, a1, sill, spring, rise) {a0, a1, sill, spring, KIT_ARCH_POINTED, rise}
#define UP_SILL                            (FLOOR2_Y + 0.85f)
#define UP_SPRING                          (FLOOR2_Y + 1.85f)
#define HALL_SILL                          (FLOOR_Y + 1.2f)
#define HALL_SPRING                        (FLOOR_Y + 4.1f)
#define DOOR2_SPRING                       (FLOOR2_Y + 2.0f)
// An upstairs doorway's foot, under the boards: the floors either side meet in it at the wall's
// middle, and a sill level with them would be a third face in their plane.
#define DOOR2_SILL (FLOOR2_Y - 0.02f)

/*
 * Every wall of the house that has a face worth dressing, in one table, so what is built and
 * what is laid on it later -- panelling, a stone lining -- read the same openings.
 */
static const KitWall WALLS[HOUSE_WALL_COUNT] = {
    // The front: the door under a pointed head, the dining room's pointed window down to its
    // seat, and upstairs a pair of lancets into the bedroom and one into the box room.
    [HOUSE_WALL_FRONT] =
        {.along_x = true,
         .at = HOUSE_FRONT_Z,
         .from = FRONT_FROM,
         .to = HOUSE_X1 + CORNER,
         .y0 = 0.0f,
         .y1 = EAVE_Y,
         .thick = EXT_WALL,
         .inner = 1,
         .mat_inner = MAT_PLASTER,
         .mat_outer = MAT_SIDING_DARK,
         .openings = {[OPENING_FRONT_DOOR] = {FRONT_DOOR_X0, FRONT_DOOR_X1, FLOOR_Y,
                                              FRONT_DOOR_SPRING, KIT_ARCH_POINTED, FRONT_DOOR_RISE,
                                              true},
                      [OPENING_DINING_WINDOW] = {KITCHEN_WIN_X0, KITCHEN_WIN_X1, DINING_WIN_SILL,
                                                 DINING_WIN_SPRING, KIT_ARCH_POINTED,
                                                 DINING_WIN_RISE},
                      LANCET(1.75f, 2.35f, UP_SILL, UP_SPRING, 0.6f),
                      LANCET(2.65f, 3.25f, UP_SILL, UP_SPRING, 0.6f),
                      LANCET(-1.0f, -0.5f, UP_SILL + 0.1f, UP_SPRING, 0.5f)},
         .opening_count = 5},
    // The back, either side of the hearth: two tall lancets into the great hall.
    [HOUSE_WALL_BACK] = {.along_x = true,
                         .at = HOUSE_BACK_Z,
                         .from = HOUSE_X0 - CORNER,
                         .to = HOUSE_X1 + CORNER,
                         .y0 = 0.0f,
                         .y1 = EAVE_Y,
                         .thick = EXT_WALL,
                         .inner = -1,
                         .mat_inner = MAT_PLASTER,
                         .mat_outer = MAT_SIDING_DARK,
                         .openings = {LANCET(-4.1f, -3.3f, HALL_SILL, HALL_SPRING, 1.0f),
                                      LANCET(1.9f, 2.7f, HALL_SILL, HALL_SPRING, 1.0f)},
                         .opening_count = 2},
    // The west side, from the tower back: two more into the great hall.
    [HOUSE_WALL_WEST] = {.along_x = false,
                         .at = HOUSE_X0,
                         .from = WEST_FROM,
                         .to = HOUSE_BACK_Z,
                         .y0 = 0.0f,
                         .y1 = EAVE_Y,
                         .thick = EXT_WALL,
                         .inner = 1,
                         .mat_inner = MAT_PLASTER,
                         .mat_outer = MAT_SIDING_DARK,
                         .openings = {LANCET(15.1f, 15.9f, HALL_SILL, HALL_SPRING, 1.0f),
                                      LANCET(17.3f, 18.1f, HALL_SILL, HALL_SPRING, 1.0f)},
                         .opening_count = 2},
    // The east side: one over the stair, lit as you climb, and the bedroom's.
    [HOUSE_WALL_EAST] = {.along_x = false,
                         .at = HOUSE_X1,
                         .from = HOUSE_FRONT_Z,
                         .to = HOUSE_BACK_Z,
                         .y0 = 0.0f,
                         .y1 = EAVE_Y,
                         .thick = EXT_WALL,
                         .inner = -1,
                         .mat_inner = MAT_PLASTER,
                         .mat_outer = MAT_SIDING_DARK,
                         .openings = {LANCET(16.6f, 17.4f, FLOOR_Y + 2.0f, FLOOR_Y + 4.3f, 0.9f),
                                      LANCET(11.6f, 12.2f, UP_SILL, UP_SPRING, 0.6f)},
                         .opening_count = 2},
    // Downstairs, the hall's two sides: the kitchen's door in one, the parlour's in the other.
    [HOUSE_WALL_HALL_EAST] = {.along_x = false,
                              .at = HALL_X1,
                              .from = HOUSE_FRONT_Z,
                              .to = KITCHEN_BACK_Z,
                              .y0 = FLOOR_Y,
                              .y1 = CEIL_Y,
                              .thick = INT_WALL,
                              .inner = 1,
                              .mat_inner = MAT_PLASTER,
                              .mat_outer = MAT_PLASTER,
                              .openings = {[OPENING_DINING_DOOR] = {KITCHEN_DOOR_Z0,
                                                                    KITCHEN_DOOR_Z1, FLOOR_Y,
                                                                    DOOR_HEAD, KIT_ARCH_FLAT,
                                                                    0.0f, true}},
                              .opening_count = 1},
    [HOUSE_WALL_HALL_WEST] = {.along_x = false,
                              .at = HALL_X0,
                              .from = HOUSE_FRONT_Z,
                              .to = KITCHEN_BACK_Z,
                              .y0 = FLOOR_Y,
                              .y1 = CEIL_Y,
                              .thick = INT_WALL,
                              .inner = 1,
                              .mat_inner = MAT_PLASTER,
                              .mat_outer = MAT_PLASTER,
                              .openings = {[OPENING_PARLOUR_DOOR] = {10.55f, 11.4f, FLOOR_Y,
                                                                     FLOOR_Y + 1.9f,
                                                                     KIT_ARCH_POINTED, 0.4f, true}},
                              .opening_count = 1},
    // Upstairs, the same two lines part the study, the box room and the bedroom.
    [HOUSE_WALL_UP_EAST] = {.along_x = false,
                            .at = HALL_X1,
                            .from = HOUSE_FRONT_Z,
                            .to = KITCHEN_BACK_Z,
                            .y0 = FLOOR2_Y,
                            .y1 = CEIL2_Y,
                            .thick = INT_WALL,
                            .inner = 1,
                            .mat_inner = MAT_PLASTER,
                            .mat_outer = MAT_PLASTER},
    [HOUSE_WALL_UP_WEST] = {.along_x = false,
                            .at = HALL_X0,
                            .from = HOUSE_FRONT_Z,
                            .to = KITCHEN_BACK_Z,
                            .y0 = FLOOR2_Y,
                            .y1 = CEIL2_Y,
                            .thick = INT_WALL,
                            .inner = 1,
                            .mat_inner = MAT_PLASTER,
                            .mat_outer = MAT_PLASTER},
    // The great hall's front, through both storeys: the hall's arch under the gallery, and off
    // the gallery the study's open doorway and the box room's and the bedroom's, shut.
    [HOUSE_WALL_GREAT_FRONT] =
        {.along_x = true,
         .at = KITCHEN_BACK_Z,
         .from = HOUSE_X0,
         .to = HOUSE_X1,
         .y0 = FLOOR_Y,
         .y1 = EAVE_Y,
         .thick = INT_WALL,
         .inner = -1,
         .mat_inner = MAT_PLASTER,
         .mat_outer = MAT_PLASTER,
         .openings = {[OPENING_GREAT_ARCH] = {HALL_IN_X0, HALL_IN_X1, FLOOR_Y, FLOOR_Y + 2.0f,
                                              KIT_ARCH_TUDOR, 0.5f, true},
                      [OPENING_STUDY_DOOR] = {-3.0f, -2.1f, DOOR2_SILL, DOOR2_SPRING,
                                              KIT_ARCH_POINTED, 0.45f, true},
                      [OPENING_BOX_ROOM_DOOR] = {-1.2f, -0.35f, DOOR2_SILL, DOOR2_SPRING,
                                                 KIT_ARCH_POINTED, 0.45f, true},
                      [OPENING_BEDROOM_DOOR] = {1.6f, 2.45f, DOOR2_SILL, DOOR2_SPRING,
                                                KIT_ARCH_POINTED, 0.45f, true}},
         .opening_count = 4},
};

const KitWall* house_wall(HouseWall which) {
    return &WALLS[which];
}

/*
 * An outside wall's dressing between a0 and a1: the base from corner to corner, battens up to
 * the eave clear of the corners, every opening cased and hooded, and a frieze board along the
 * eave that the gable's boards stand on.
 */
static void dress(Kit* kit, const KitWall* w, float a0, float a1) {
    const Facade s = facade_of(w);
    ornament_base(kit, &s, a0, a1, w->openings, w->opening_count);
    ornament_battens(kit, &s, a0 + 0.1f, a1 - 0.1f, BOARDS_Y, EAVE_Y - 0.2f, w->openings,
                     w->opening_count);
    for (int i = 0; i < w->opening_count; i++)
        ornament_window(kit, &s, &w->openings[i]);
    const float f = s.face, o = s.out, y = EAVE_Y - 0.24f;
    const vec2 frieze[4] = {{f, y}, {f + o * 0.05f, y}, {f + o * 0.05f, y + 0.28f}, {f, y + 0.28f}};
    kit_frame_run(kit, &s.f, MAT_SIDING_DARK, frieze, 4, a0, a1);
}

#define NO_PANE (-1)
#define LAP     (CORNER + 0.05f)

/*
 * The outside walls: what glazes each opening, and where each wall's dressing runs -- from the
 * tower's face, or past the corner it shares. The dining room's window and the great hall's
 * lancets take leaded quarries, the one over the stair too, and the rooms upstairs are dark.
 */
static const struct {
    HouseWall wall;
    int glass[KIT_MAX_OPENINGS];
    float dress_from, dress_to;
} OUTSIDE[] = {
    {HOUSE_WALL_FRONT,
     {[OPENING_FRONT_DOOR] = NO_PANE,
      [OPENING_DINING_WINDOW] = MAT_LEADED,
      MAT_DARK_GLASS,
      MAT_DARK_GLASS,
      MAT_DARK_GLASS},
     FRONT_FROM + CORNER,
     HOUSE_X1 + LAP},
    {HOUSE_WALL_BACK, {MAT_LEADED, MAT_LEADED}, HOUSE_X0 - LAP, HOUSE_X1 + LAP},
    {HOUSE_WALL_WEST, {MAT_LEADED, MAT_LEADED}, WEST_FROM + CORNER, HOUSE_BACK_Z + LAP},
    {HOUSE_WALL_EAST, {MAT_LEADED, MAT_DARK_GLASS}, HOUSE_FRONT_Z - LAP, HOUSE_BACK_Z + LAP},
};

static void exterior_walls(Kit* kit) {
    for (int k = 0; k < KIT_COUNT(OUTSIDE); k++) {
        const KitWall* w = &WALLS[OUTSIDE[k].wall];
        kit_wall(kit, w);
        for (int i = 0; i < w->opening_count; i++)
            if (OUTSIDE[k].glass[i] != NO_PANE)
                pane_in(kit, w, i, OUTSIDE[k].glass[i]);
        dress(kit, w, OUTSIDE[k].dress_from, OUTSIDE[k].dress_to);
    }
    // Inside, the front door's stone threshold, under the leaf.
    const KitOpening* door = &WALLS[HOUSE_WALL_FRONT].openings[OPENING_FRONT_DOOR];
    kit_frame_box(kit, &KIT_WORLD, MAT_STONE, door->from, door->to, FLOOR_Y - 0.02f,
                  FLOOR_Y + 0.015f, HOUSE_FRONT_Z - CORNER - 0.04f, HOUSE_FRONT_Z + CORNER, false);
}

static void interior_walls(Kit* kit) {
    kit_wall(kit, &WALLS[HOUSE_WALL_HALL_EAST]);
    // The parlour's door seen from the hall, the wall's inner side; the box room's and the
    // bedroom's from the gallery, the great hall's front's outer side.
    kit_wall(kit, &WALLS[HOUSE_WALL_HALL_WEST]);
    door_in(kit, &WALLS[HOUSE_WALL_HALL_WEST], OPENING_PARLOUR_DOOR, true);
    kit_wall(kit, &WALLS[HOUSE_WALL_UP_EAST]);
    kit_wall(kit, &WALLS[HOUSE_WALL_UP_WEST]);
    kit_wall(kit, &WALLS[HOUSE_WALL_GREAT_FRONT]);
    door_in(kit, &WALLS[HOUSE_WALL_GREAT_FRONT], OPENING_BOX_ROOM_DOOR, false);
    door_in(kit, &WALLS[HOUSE_WALL_GREAT_FRONT], OPENING_BEDROOM_DOOR, false);
}

/*
 * Floors and ceilings. The front band is a sandwich -- the ceiling under, the boards over --
 * and every slab in it leaves the tower out, whose own are octagons (tower.c); its bodies
 * are plain boxes, which may run into the tower since its floors are at the same heights.
 */
static void floors(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HALL_X0, HOUSE_X1, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
                  KITCHEN_BACK_Z, true);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HOUSE_X0, HOUSE_X1, 0.0f, FLOOR_Y, KITCHEN_BACK_Z,
                  HOUSE_BACK_Z, true);

    // The parlour's boards round the tower's.
    vec2 notch[KIT_MAX_OUTLINE];
    int n = tower_notch(HOUSE_X0, HOUSE_FRONT_Z, HALL_X0, KITCHEN_BACK_Z, TOWER_APOTHEM, notch);
    kit_slab(kit, MAT_WOOD_FLOOR, notch, n, 0.0f, FLOOR_Y, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, HOUSE_X0, HALL_X0, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
                  KITCHEN_BACK_Z, true);

    // Between the storeys, over the whole front band, and the attic's floor over that.
    n = tower_notch(HOUSE_X0, HOUSE_FRONT_Z, HOUSE_X1, KITCHEN_BACK_Z, TOWER_APOTHEM, notch);
    kit_slab(kit, MAT_CEILING, notch, n, CEIL_Y, CEIL_Y + SLAB, false);
    kit_slab(kit, MAT_WOOD_FLOOR, notch, n, CEIL_Y + SLAB, FLOOR2_Y, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, HOUSE_X0, HOUSE_X1, CEIL_Y, FLOOR2_Y, HOUSE_FRONT_Z,
                  KITCHEN_BACK_Z, true);
    // The upper ceiling runs in under the tower's walls to their inner face, and the walls that
    // close the study's tall bay off from the attic stand on it: walls starting level with its
    // underside put their feet in that plane, and the two fought.
    n = tower_notch(HOUSE_X0, HOUSE_FRONT_Z, HOUSE_X1, KITCHEN_BACK_Z,
                    TOWER_APOTHEM - 0.5f * EXT_WALL, notch);
    kit_slab(kit, MAT_CEILING, notch, n, CEIL2_Y, CEIL2_Y + SLAB, false);

    // The gallery, the same sandwich out over the great hall; its balustrade is interior.c's.
    kit_frame_box(kit, w, MAT_CEILING, HOUSE_X0, HOUSE_X1, CEIL_Y, CEIL_Y + SLAB, KITCHEN_BACK_Z,
                  GALLERY_Z1, false);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HOUSE_X0, HOUSE_X1, CEIL_Y + SLAB, FLOOR2_Y,
                  KITCHEN_BACK_Z, GALLERY_Z1, true);
}

// The stair up the great hall's east wall, from its foot near the back to the gallery.
static void stair(Kit* kit) {
    // Turned half round, so d climbs toward -z and a runs west off the wall.
    const KitFrame f = {{GREAT_X1, 0.0f, STAIR_FOOT_Z}, GLM_PIf};
    kit_frame_stair(kit, &f, MAT_MAHOGANY, 0.0f, GREAT_X1 - STAIR_X0, FLOOR_Y, 0.0f, STAIR_RISE,
                    STAIR_GOING, STAIR_RISERS);
}

/*
 * The hearth's chimney, outside the back wall: a broad breast up the wall, weathered in to a
 * shaft that stands clear of the roof's rake and rises past the ridge, a corbelled cap, and two
 * clay pots.
 */
#define STACK_TOP (house_roof_y(0.0f) + 0.65f)

static void chimney(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    const float x = HEARTH_X, wall = HOUSE_BACK_Z + CORNER;
    const float shaft0 = HOUSE_BACK_Z + CORNER + RAKE_OVERHANG + 0.06f, shaft1 = shaft0 + 0.5f;
    const float breast_top = EAVE_Y - 0.6f;
    kit_frame_box(kit, w, MAT_STONE, x - 0.8f, x + 0.8f, 0.0f, breast_top, wall, shaft1, true);
    for (int s = -1; s <= 1; s += 2) {
        const float edge = x + 0.8f * (float)s, inner = x + 0.5f * (float)s;
        const vec2 weathering[3] = {
            {edge, breast_top}, {inner, breast_top}, {inner, breast_top + 0.45f}};
        kit_frame_extrude(kit, w, MAT_STONE, weathering, 3, wall, shaft1);
    }
    kit_frame_box(kit, w, MAT_STONE, x - 0.5f, x + 0.5f, breast_top, STACK_TOP, shaft0, shaft1,
                  false);
    kit_frame_box(kit, w, MAT_STONE, x - 0.62f, x + 0.62f, STACK_TOP, STACK_TOP + 0.14f,
                  shaft0 - 0.12f, shaft1 + 0.12f, false);
    kit_frame_box(kit, w, MAT_STONE, x - 0.54f, x + 0.54f, STACK_TOP + 0.14f, STACK_TOP + 0.24f,
                  shaft0 - 0.04f, shaft1 + 0.04f, false);
    // Open at the top, so its rim shows a lip and the dark inside.
    const vec2 pot[] = {{0.0f, 0.0f},  {0.12f, 0.0f}, {0.12f, 0.06f}, {0.09f, 0.1f}, {0.08f, 0.4f},
                        {0.1f, 0.44f}, {0.1f, 0.5f},  {0.075f, 0.5f}, {0.075f, 0.3f}};
    for (int s = -1; s <= 1; s += 2)
        kit_frame_lathe(kit, w, MAT_BRICK, x + 0.22f * (float)s, 0.5f * (shaft0 + shaft1),
                        STACK_TOP + 0.24f, pot, (int)(sizeof(pot) / sizeof(pot[0])), 12);
}

static void porch(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, FLOOR_Y, PORCH_Z0, HOUSE_FRONT_Z,
                  true);
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, 0.5f * FLOOR_Y, PORCH_Z0 - 0.32f,
                  PORCH_Z0, true);
}

void house_build(Kit* kit) {
    exterior_walls(kit);
    interior_walls(kit);
    floors(kit);
    stair(kit);
    main_roof(kit);
    pent_roof(kit);
    chimney(kit);
    porch(kit);
    tower_build(kit);
}

/*
 * The front door, hung on its west jamb in the inner half of the wall so it swings into the
 * hall and stands open against it, short of the hall's wall. It fills its opening shy of a
 * leaf's clearance all round.
 */
#define DOOR_CLEARANCE 0.008f
#define DOOR_SWING     1.7f // about 97 degrees

bool house_front_door(Door* door, Engine* engine, Scene* scene, EntityManager* em,
                      PhysicsWorld* physics, const vec3 origin) {
    // The opening as the wall has it, along the frame from its hinge jamb.
    KitOpening opening = WALLS[HOUSE_WALL_FRONT].openings[OPENING_FRONT_DOOR];
    const KitFrame hinge = {{opening.from + origin[0], origin[1], FRONT_DOOR_Z + origin[2]}, 0.0f};
    opening.to -= opening.from;
    opening.from = 0.0f;
    KitOpening leaf = kit_opening_grow(&opening, -DOOR_CLEARANCE);
    leaf.bottom = FLOOR_Y + 0.02f;
    return door_build(door, engine, scene, em, physics, "mansion_door", door_leaf, &hinge, &leaf,
                      DOOR_THICK, DOOR_SWING);
}

float house_clearance(const vec3 p) {
    float d = tower_wall_distance(p);
    for (int i = 0; i < HOUSE_WALL_COUNT; i++) {
        const KitWall* w = &WALLS[i];
        const vec2 a = {w->along_x ? w->from : w->at, w->along_x ? w->at : w->from};
        const vec2 b = {w->along_x ? w->to : w->at, w->along_x ? w->at : w->to};
        d = fminf(d, kit_plan_distance(p, a, b) - 0.5f * w->thick);
    }
    // The floor and the ceilings, each as a band of height over the whole plan.
    static const float SLABS[][2] = {{0.0f, FLOOR_Y},
                                     {CEIL_Y, FLOOR2_Y},
                                     {CEIL2_Y, CEIL2_Y + SLAB},
                                     {TOWER_CEIL_Y, TOWER_CEIL_Y + SLAB}};
    for (int i = 0; i < KIT_COUNT(SLABS); i++)
        d = fminf(d, fmaxf(SLABS[i][0] - p[1], p[1] - SLABS[i][1]));
    return d;
}

float house_outside_distance(const vec3 p) {
    const float cx = 0.5f * (HOUSE_X0 + HOUSE_X1), cz = 0.5f * (HOUSE_FRONT_Z + HOUSE_BACK_Z);
    // To the outer faces, as the tower's is: HOUSE_* are the walls' middles.
    const float hx = 0.5f * (HOUSE_X1 - HOUSE_X0) + CORNER;
    const float hz = 0.5f * (HOUSE_BACK_Z - HOUSE_FRONT_Z) + CORNER;
    const float body = fmaxf(fabsf(p[0] - cx) - hx, fabsf(p[2] - cz) - hz);
    return fminf(body, tower_outside_distance(p));
}
