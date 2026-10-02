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

/*
 * The skirt roof across the front at the upper floor, PENT_PITCH a metre: a deep one over the
 * porch, and a strip past it over the kitchen window, shallow enough that the wind still drives
 * the rain onto the window's lower half.
 */
#define PENT_Y        3.3f // where it meets the wall
#define PENT_PITCH    0.47f
#define PENT_DEPTH    0.45f
#define PORCH_ROOF_X1 0.9f
#define PENT_THICK    0.08f

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

// The main roof's height over (x, z): its ridge on x = 0.
static float main_roof_y(float x) {
    return EAVE_Y + (HOUSE_X1 + CORNER - fabsf(x)) * MAIN_PITCH;
}

/*
 * A roof slab under the plane y = y0 + gx x + gz z, over the outline `xz` in plan: slate on
 * top, and a roof's thickness under it the soffit, with a fascia down every edge.
 */
static void roof_slab(Kit* kit, const vec2* xz, int count, float y0, float gx, float gz,
                      float thick) {
    vec3 top[KIT_MAX_OUTLINE] = {{0.0f}}, under[KIT_MAX_OUTLINE] = {{0.0f}};
    float area = 0.0f;
    for (int i = 0; i < count; i++) {
        const float y = y0 + gx * xz[i][0] + gz * xz[i][1];
        glm_vec3_copy((vec3){xz[i][0], y, xz[i][1]}, top[i]);
        glm_vec3_copy((vec3){xz[i][0], y - thick, xz[i][1]}, under[i]);
        const float* q = xz[(i + 1) % count];
        area += xz[i][0] * q[1] - q[0] * xz[i][1];
    }
    kit_polygon_facing(kit, MAT_SLATE, top, count, (vec3){-gx, 1.0f, -gz});
    kit_polygon_facing(kit, MAT_SIDING_DARK, under, count, (vec3){gx, -1.0f, gz});
    // Out of the outline is to the right of each edge when it runs counter-clockwise.
    const float turn = area > 0.0f ? 1.0f : -1.0f;
    for (int i = 0; i < count; i++) {
        const int j = (i + 1) % count;
        const vec3 out = {turn * (xz[j][1] - xz[i][1]), 0.0f, -turn * (xz[j][0] - xz[i][0])};
        kit_quad_facing(kit, MAT_SIDING_DARK, top[i], top[j], under[j], under[i], out);
    }
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
    const float ridge = main_roof_y(0.0f);
    vec2 west[KIT_MAX_OUTLINE];
    const int n = tower_notch(-x_eave, z0, 0.0f, z1, TOWER_APOTHEM + 0.5f * EXT_WALL, west);
    roof_slab(kit, west, n, ridge, MAIN_PITCH, 0.0f, ROOF_THICK);
    const vec2 east[4] = {{0.0f, z0}, {x_eave, z0}, {x_eave, z1}, {0.0f, z1}};
    roof_slab(kit, east, 4, ridge, -MAIN_PITCH, 0.0f, ROOF_THICK);

    const float side = HOUSE_X1 + CORNER;
    const vec2 front[4] = {
        {FRONT_FROM, EAVE_Y}, {side, EAVE_Y}, {0.0f, ridge}, {FRONT_FROM, main_roof_y(FRONT_FROM)}};
    kit_frame_extrude(kit, &KIT_WORLD, MAT_SIDING_DARK, front, 4, HOUSE_FRONT_Z - CORNER,
                      HOUSE_FRONT_Z + CORNER);
    const vec2 back[3] = {{-side, EAVE_Y}, {side, EAVE_Y}, {0.0f, ridge}};
    kit_frame_extrude(kit, &KIT_WORLD, MAT_SIDING_DARK, back, 3, HOUSE_BACK_Z - CORNER,
                      HOUSE_BACK_Z + CORNER);
    const vec2 inner[5] = {{HOUSE_X0, EAVE_Y},
                           {HOUSE_X1, EAVE_Y},
                           {HOUSE_X1, main_roof_y(HOUSE_X1)},
                           {0.0f, ridge},
                           {HOUSE_X0, main_roof_y(HOUSE_X0)}};
    kit_frame_extrude(kit, &KIT_WORLD, MAT_PLASTER, inner, 5, KITCHEN_BACK_Z - 0.5f * INT_WALL,
                      KITCHEN_BACK_Z + 0.5f * INT_WALL);

    // The gables dressed: boards up to the rakes, a cusped bargeboard down each one at the
    // roof's edge, and a finial at each apex with its pendant; iron cresting along the ridge.
    const Facade fronts[2] = {{KIT_WORLD, HOUSE_FRONT_Z - CORNER, HOUSE_FRONT_Z, -1.0f},
                              {KIT_WORLD, HOUSE_BACK_Z + CORNER, HOUSE_BACK_Z, 1.0f}};
    const float edges[2] = {z0, z1};
    const float tower_face = FRONT_FROM + CORNER;
    for (int g = 0; g < 2; g++) {
        const float from = g == 0 ? tower_face : -side;
        ornament_gable_battens(kit, &fronts[g], from + 0.1f, side - 0.1f, EAVE_Y + 0.04f, 0.0f,
                               ridge, MAIN_PITCH);
        const float d = edges[g];
        const vec2 apex = {0.0f, ridge + 0.03f};
        const vec2 right = {x_eave, main_roof_y(x_eave) + 0.03f};
        const float left_x = g == 0 ? tower_face : -x_eave;
        const vec2 left = {left_x, main_roof_y(left_x) + 0.03f};
        ornament_bargeboard(kit, &KIT_WORLD, right, apex, d - 0.02f, d + 0.02f);
        ornament_bargeboard(kit, &KIT_WORLD, left, apex, d - 0.02f, d + 0.02f);
        ornament_finial(kit, &KIT_WORLD, MAT_SIDING_DARK, 0.0f, ridge + 0.03f, d, 1.2f, 0.7f);
    }
    const KitFrame ridge_line = {{0.0f, 0.0f, 0.0f}, -0.5f * GLM_PIf};
    ornament_cresting(kit, &ridge_line, z0 + 0.3f, z1 - 0.3f, ridge, 0.0f);

    // The bare side eaves drip, the west one from past the tower.
    const float drip_y = main_roof_y(x_eave) - ROOF_THICK;
    const float west_from = TOWER_Z + TOWER_APOTHEM + 0.5f * EXT_WALL + 0.1f;
    kit_drip(kit, &KIT_WORLD, (vec3){-x_eave, drip_y, west_from}, (vec3){-x_eave, drip_y, z1},
             EAVE_DRIPS_PER_M * (z1 - west_from), 0.0f);
    kit_drip(kit, &KIT_WORLD, (vec3){x_eave, drip_y, z0}, (vec3){x_eave, drip_y, z1},
             EAVE_DRIPS_PER_M * (z1 - z0), 0.0f);
}

/*
 * A gutter hung under the pent strip's edge, which is `edge_y` high at z = `fascia`, from x0
 * to x1: a channel open at the top, and a downpipe at the east end that drops to the wall and
 * down it to a shoe kicked out over the yard. It leaks at a joint by the porch, over the
 * kitchen window where it sags, and from the shoe.
 */
static void gutter(Kit* kit, float x0, float x1, float edge_y, float fascia) {
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
        ornament_post(kit, &KIT_WORLD, posts[i], post_z, FLOOR_Y, post_top, 0.06f);
    const float spring = post_top - 0.5f;
    ornament_arch_board(kit, &KIT_WORLD, MAT_SIDING_DARK, posts[0] + 0.07f, posts[1] - 0.07f,
                        spring, post_top, KIT_ARCH_TUDOR, 0.36f, post_z);
    const KitFrame side = {{0.0f, 0.0f, 0.0f}, -0.5f * GLM_PIf}; // a runs along z, d along -x
    for (int i = 0; i < 2; i++)
        ornament_arch_board(kit, &side, MAT_SIDING_DARK, post_z + 0.07f, wall, spring, post_top,
                            KIT_ARCH_POINTED, 0.36f, -posts[i]);
    const float step0 = -1.3f, step1 = -0.25f; // the gap in front of the door, and the path
    ornament_balustrade(kit, &KIT_WORLD, posts[0] + 0.08f, step0, FLOOR_Y, post_z);
    ornament_balustrade(kit, &KIT_WORLD, step1, posts[1] - 0.08f, FLOOR_Y, post_z);
    for (int i = 0; i < 2; i++)
        ornament_balustrade(kit, &side, post_z + 0.08f, wall - 0.02f, FLOOR_Y, -posts[i]);
    kit_drip(kit, &KIT_WORLD, (vec3){tower_face + 0.1f, porch_under, porch_edge},
             (vec3){PORCH_ROOF_X1 - 0.05f, porch_under, porch_edge},
             PORCH_DRIPS_PER_M * (PORCH_ROOF_X1 - tower_face), 0.0f);
    gutter(kit, PORCH_ROOF_X1 + 0.05f, x_end - 0.05f, y0 + PENT_PITCH * strip_edge - PENT_THICK,
           strip_edge);
}

// A pane of `glass` in opening `i` of the axis-aligned wall `w`.
static void pane_in(Kit* kit, const KitWall* w, int i, int glass) {
    KitFrame f;
    float at = 0.0f;
    kit_wall_frame(w, &f, &at);
    kit_frame_pane(kit, &f, glass, &w->openings[i], at, w->thick);
}

// A shut door filling opening `i` of the axis-aligned wall `w`, 5 cm thick, and its body.
static void door_in(Kit* kit, const KitWall* w, int i) {
    KitFrame f;
    float at = 0.0f;
    kit_wall_frame(w, &f, &at);
    const KitOpening* o = &w->openings[i];
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(o, outline);
    kit_frame_extrude(kit, &f, MAT_WOOD, outline, n, at - 0.025f, at + 0.025f);
    kit_frame_box(kit, &f, KIT_COLLIDER_ONLY, o->from, o->to, o->bottom, o->top + o->rise,
                  at - 0.5f * w->thick, at + 0.5f * w->thick, true);
}

// A lancet from a0 to a1 along a wall, its sill at `sill`, springing at `spring` and rising
// `rise` to its point.
static KitOpening lancet(float a0, float a1, float sill, float spring, float rise) {
    return (KitOpening){a0, a1, sill, spring, KIT_ARCH_POINTED, rise};
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

static void exterior_walls(Kit* kit) {
    // The front: the door under a pointed head, the kitchen's window as it was, and upstairs
    // a pair of lancets into the bedroom and one into the box room.
    const float up_sill = FLOOR2_Y + 0.85f, up_spring = FLOOR2_Y + 1.85f;
    const KitWall front = {
        .along_x = true,
        .at = HOUSE_FRONT_Z,
        .from = FRONT_FROM,
        .to = HOUSE_X1 + CORNER,
        .y0 = 0.0f,
        .y1 = EAVE_Y,
        .thick = EXT_WALL,
        .inner = 1,
        .mat_inner = MAT_PLASTER,
        .mat_outer = MAT_SIDING_DARK,
        .openings = {{FRONT_DOOR_X0, FRONT_DOOR_X1, FLOOR_Y, FRONT_DOOR_SPRING, KIT_ARCH_POINTED,
                      FRONT_DOOR_RISE},
                     {KITCHEN_WIN_X0, KITCHEN_WIN_X1, KITCHEN_WIN_SILL, KITCHEN_WIN_HEAD},
                     lancet(1.75f, 2.35f, up_sill, up_spring, 0.6f),
                     lancet(2.65f, 3.25f, up_sill, up_spring, 0.6f),
                     lancet(-1.0f, -0.5f, up_sill + 0.1f, up_spring, 0.5f)},
        .opening_count = 5};
    kit_wall(kit, &front);
    pane_in(kit, &front, 1, MAT_WINDOW_GLASS);
    for (int i = 2; i < front.opening_count; i++)
        pane_in(kit, &front, i, MAT_DARK_GLASS);
    // Inside, the front door's oak casing and its stone threshold; the leaf is door.c's.
    const KitOpening* door = &front.openings[0];
    const float inner = HOUSE_FRONT_Z + CORNER;
    kit_frame_surround(kit, &KIT_WORLD, MAT_WOOD, door, 0.08f, false, inner, inner + 0.03f);
    kit_frame_box(kit, &KIT_WORLD, MAT_STONE, door->from, door->to, FLOOR_Y - 0.02f,
                  FLOOR_Y + 0.015f, HOUSE_FRONT_Z - CORNER - 0.04f, inner, false);
    // Each wall's dressing runs from the tower's face, or past the corner it shares.
    const float lap = CORNER + 0.05f;
    dress(kit, &front, FRONT_FROM + CORNER, HOUSE_X1 + lap);

    // The back, either side of the hearth: two tall lancets into the great hall.
    const float hall_sill = FLOOR_Y + 1.2f, hall_spring = FLOOR_Y + 4.1f;
    KitWall back = front;
    back.at = HOUSE_BACK_Z;
    back.from = HOUSE_X0 - CORNER;
    back.inner = -1;
    back.openings[0] = lancet(-4.1f, -3.3f, hall_sill, hall_spring, 1.0f);
    back.openings[1] = lancet(1.9f, 2.7f, hall_sill, hall_spring, 1.0f);
    back.opening_count = 2;
    kit_wall(kit, &back);
    pane_in(kit, &back, 0, MAT_WINDOW_GLASS);
    pane_in(kit, &back, 1, MAT_WINDOW_GLASS);
    dress(kit, &back, HOUSE_X0 - lap, HOUSE_X1 + lap);

    // The west side, from the tower back: two more into the great hall.
    const KitWall west = {.along_x = false,
                          .at = HOUSE_X0,
                          .from = WEST_FROM,
                          .to = HOUSE_BACK_Z,
                          .y0 = 0.0f,
                          .y1 = EAVE_Y,
                          .thick = EXT_WALL,
                          .inner = 1,
                          .mat_inner = MAT_PLASTER,
                          .mat_outer = MAT_SIDING_DARK,
                          .openings = {lancet(15.1f, 15.9f, hall_sill, hall_spring, 1.0f),
                                       lancet(17.3f, 18.1f, hall_sill, hall_spring, 1.0f)},
                          .opening_count = 2};
    kit_wall(kit, &west);
    pane_in(kit, &west, 0, MAT_WINDOW_GLASS);
    pane_in(kit, &west, 1, MAT_WINDOW_GLASS);
    dress(kit, &west, WEST_FROM + CORNER, HOUSE_BACK_Z + lap);

    // The east side: one over the stair, lit as you climb, and the bedroom's.
    KitWall east = west;
    east.at = HOUSE_X1;
    east.from = HOUSE_FRONT_Z;
    east.inner = -1;
    east.openings[0] = lancet(16.6f, 17.4f, FLOOR_Y + 2.0f, FLOOR_Y + 4.3f, 0.9f);
    east.openings[1] = lancet(11.6f, 12.2f, up_sill, up_spring, 0.6f);
    east.opening_count = 2;
    kit_wall(kit, &east);
    pane_in(kit, &east, 0, MAT_WINDOW_GLASS);
    pane_in(kit, &east, 1, MAT_DARK_GLASS);
    dress(kit, &east, HOUSE_FRONT_Z - lap, HOUSE_BACK_Z + lap);
}

static void interior_walls(Kit* kit) {
    // Downstairs: the hall's two sides, the kitchen's door in one and the parlour's, shut, in
    // the other.
    KitWall hall_east = {.along_x = false,
                         .at = HALL_X1,
                         .from = HOUSE_FRONT_Z,
                         .to = KITCHEN_BACK_Z,
                         .y0 = FLOOR_Y,
                         .y1 = CEIL_Y,
                         .thick = INT_WALL,
                         .inner = 1,
                         .mat_inner = MAT_PLASTER,
                         .mat_outer = MAT_PLASTER,
                         .openings = {{KITCHEN_DOOR_Z0, KITCHEN_DOOR_Z1, FLOOR_Y, DOOR_HEAD}},
                         .opening_count = 1};
    kit_wall(kit, &hall_east);
    KitWall hall_west = hall_east;
    hall_west.at = HALL_X0;
    hall_west.openings[0] =
        (KitOpening){10.55f, 11.4f, FLOOR_Y, FLOOR_Y + 1.9f, KIT_ARCH_POINTED, 0.4f};
    kit_wall(kit, &hall_west);
    door_in(kit, &hall_west, 0);

    // Upstairs, the same two lines part the study, the box room and the bedroom.
    KitWall up = hall_east;
    up.y0 = FLOOR2_Y;
    up.y1 = CEIL2_Y;
    up.opening_count = 0;
    kit_wall(kit, &up);
    up.at = HALL_X0;
    kit_wall(kit, &up);

    // The great hall's front, through both storeys: the hall's arch under the gallery, and
    // off the gallery the study's open doorway and the other two shut.
    const float up_spring = FLOOR2_Y + 2.0f;
    const KitWall great_front = {
        .along_x = true,
        .at = KITCHEN_BACK_Z,
        .from = HOUSE_X0,
        .to = HOUSE_X1,
        .y0 = FLOOR_Y,
        .y1 = EAVE_Y,
        .thick = INT_WALL,
        .inner = -1,
        .mat_inner = MAT_PLASTER,
        .mat_outer = MAT_PLASTER,
        .openings = {{HALL_X0 + 0.5f * INT_WALL, HALL_X1 - 0.5f * INT_WALL, FLOOR_Y, FLOOR_Y + 2.0f,
                      KIT_ARCH_TUDOR, 0.5f},
                     {-3.0f, -2.1f, FLOOR2_Y, up_spring, KIT_ARCH_POINTED, 0.45f},
                     {-1.2f, -0.35f, FLOOR2_Y, up_spring, KIT_ARCH_POINTED, 0.45f},
                     {1.6f, 2.45f, FLOOR2_Y, up_spring, KIT_ARCH_POINTED, 0.45f}},
        .opening_count = 4};
    kit_wall(kit, &great_front);
    door_in(kit, &great_front, 2);
    door_in(kit, &great_front, 3);
}

/*
 * Floors and ceilings. The front band is a sandwich -- the ceiling under, the boards over --
 * and every slab in it leaves the tower out, whose own are octagons (tower.c); its bodies
 * are plain boxes, which may run into the tower since its floors are at the same heights.
 */
static void floors(Kit* kit) {
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_KITCHEN_FLOOR, HALL_X1, HOUSE_X1, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
                  KITCHEN_BACK_Z, true);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HALL_X0, HALL_X1, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
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
    kit_slab(kit, MAT_CEILING, notch, n, CEIL2_Y, CEIL2_Y + SLAB, false);

    // The gallery, the same sandwich out over the great hall, and its rail.
    kit_frame_box(kit, w, MAT_CEILING, HOUSE_X0, HOUSE_X1, CEIL_Y, CEIL_Y + SLAB, KITCHEN_BACK_Z,
                  GALLERY_Z1, false);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HOUSE_X0, HOUSE_X1, CEIL_Y + SLAB, FLOOR2_Y,
                  KITCHEN_BACK_Z, GALLERY_Z1, true);
    const float rail = FLOOR2_Y + 0.95f;
    kit_frame_box(kit, w, MAT_WOOD, GREAT_X0, STAIR_X0, rail - 0.06f, rail, GALLERY_Z1 - 0.05f,
                  GALLERY_Z1 + 0.05f, false);
    for (float x = GREAT_X0 + 0.04f; x < STAIR_X0; x += 1.1f)
        kit_frame_box(kit, w, MAT_WOOD, x, x + 0.08f, FLOOR2_Y, rail, GALLERY_Z1 - 0.04f,
                      GALLERY_Z1 + 0.04f, false);
    kit_frame_box(kit, w, MAT_WOOD, STAIR_X0, STAIR_X0 + 0.1f, FLOOR2_Y, rail + 0.15f,
                  GALLERY_Z1 - 0.05f, GALLERY_Z1 + 0.05f, false);
    kit_frame_box(kit, w, KIT_COLLIDER_ONLY, GREAT_X0, STAIR_X0, FLOOR2_Y, rail + 0.05f,
                  GALLERY_Z1 - 0.05f, GALLERY_Z1 + 0.05f, true);
}

// The stair up the great hall's east wall, from its foot near the back to the gallery.
static void stair(Kit* kit) {
    // Turned half round, so d climbs toward -z and a runs west off the wall.
    const KitFrame f = {{GREAT_X1, 0.0f, STAIR_FOOT_Z}, GLM_PIf};
    kit_frame_stair(kit, &f, MAT_WOOD, 0.0f, GREAT_X1 - STAIR_X0, FLOOR_Y, 0.0f, STAIR_RISE,
                    STAIR_GOING, STAIR_RISERS);
}

/*
 * The hearth's chimney, outside the back wall: a broad breast up the wall, weathered in to a
 * shaft that stands clear of the roof's rake and rises past the ridge, a corbelled cap, and two
 * clay pots.
 */
#define STACK_TOP (main_roof_y(0.0f) + 0.65f)

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
#define DOOR_THICK     0.05f
#define DOOR_CLEARANCE 0.008f
#define DOOR_SWING     1.7f // about 97 degrees

int house_doors(Door* doors, int max, Engine* engine, Scene* scene, EntityManager* em,
                PhysicsWorld* physics) {
    if (max < 1)
        return 0;
    const KitFrame hinge = {
        {FRONT_DOOR_X0, 0.0f, HOUSE_FRONT_Z + CORNER - 0.5f * DOOR_THICK - 0.005f}, 0.0f};
    const KitOpening opening = {
        0.0f,           FRONT_DOOR_X1 - FRONT_DOOR_X0, FLOOR_Y, FRONT_DOOR_SPRING, KIT_ARCH_POINTED,
        FRONT_DOOR_RISE};
    KitOpening leaf = kit_opening_grow(&opening, -DOOR_CLEARANCE);
    leaf.bottom = FLOOR_Y + 0.02f;
    door_build(&doors[0], engine, scene, em, physics, "front_door", &hinge, &leaf, DOOR_THICK,
               DOOR_SWING);
    return 1;
}

float house_outside_distance(const vec3 p) {
    const float cx = 0.5f * (HOUSE_X0 + HOUSE_X1), cz = 0.5f * (HOUSE_FRONT_Z + HOUSE_BACK_Z);
    const float hx = 0.5f * (HOUSE_X1 - HOUSE_X0), hz = 0.5f * (HOUSE_BACK_Z - HOUSE_FRONT_Z);
    const float body = fmaxf(fabsf(p[0] - cx) - hx, fabsf(p[2] - cz) - hz);
    return fminf(body, tower_outside_distance(p));
}
