#include "house.h"
#include "houses.h"
#include "layout.h"
#include "mats.h"

// A thin grimy pane in the middle of the front wall, filling an opening, and a
// body through the whole thickness so nobody climbs through.
static void pane(Kit* kit, const KitOpening* o) {
    const vec3 c = {0.5f * (o->from + o->to), 0.5f * (o->bottom + o->top), HOUSE_FRONT_Z};
    const float hx = 0.5f * (o->to - o->from), hy = 0.5f * (o->top - o->bottom);
    kit_box(kit, MAT_WINDOW_GLASS, c, (vec3){hx, hy, 0.003f}, 0.0f, false);
    kit_collider(kit, c, (vec3){hx, hy, 0.5f * EXT_WALL}, 0.0f);
}

// The downpipe, the gutter's sheet, and the leaks as drops a second at the reference rain: a
// joint seeps, the lip over the kitchen window overflows where the gutter sags, and the shoe at
// the pipe's foot runs nearly in a stream.
#define DOWNPIPE_R     0.035f
#define GUTTER_SHEET   0.015f
#define JOINT_DRIPS    3.0f
#define OVERFLOW_DRIPS 40.0f
#define SPOUT_DRIPS    40.0f

/*
 * A gutter hung on the fascia under the front eave, whose edge is `tip_y` high and `overhang`
 * out from `front`: a channel open at the top along the whole eave, its back the fascia, and a
 * downpipe by the east corner that drops to the wall and down it to a shoe kicked out over the
 * yard. It leaks at a joint over the porch by the door, at one past the front room's window,
 * over the kitchen window where it sags, and from the shoe.
 */
static void gutter(Kit* kit, const KitWall* front, float tip_y, float overhang) {
    const KitFrame* w = &KIT_WORLD;
    const float x0 = front->from - overhang + 0.04f, x1 = front->to + overhang - 0.04f;
    const float fascia = front->at - 0.5f * front->thick - overhang;
    const float bottom = tip_y - 0.14f, lip = tip_y - 0.03f, lip_z = fascia - 0.13f;
    kit_frame_box(kit, w, MAT_TRIM, x0, x1, bottom, bottom + GUTTER_SHEET, lip_z, fascia, false);
    kit_frame_box(kit, w, MAT_TRIM, x0, x1, bottom, lip, lip_z, lip_z + GUTTER_SHEET, false);
    const float ends[] = {x0, x1};
    for (int e = 0; e < 2; e++)
        kit_frame_box(kit, w, MAT_TRIM, ends[e] - 0.5f * GUTTER_SHEET,
                      ends[e] + 0.5f * GUTTER_SHEET, bottom, lip, lip_z, fascia, false);

    // Out of the gutter's floor, back to the wall in a swan neck, and down it.
    const float pipe_x = HOUSE_X1 - 0.25f;
    const float mid = 0.5f * (lip_z + fascia);
    // Held a bracket's 15 mm off the siding.
    const float wall = front->at - 0.5f * front->thick - DOWNPIPE_R - 0.015f;
    const vec3 path[] = {{pipe_x, bottom, mid},
                         {pipe_x, bottom - 0.15f, mid},
                         {pipe_x, bottom - 0.27f, wall},
                         {pipe_x, 0.3f, wall},
                         {pipe_x, 0.14f, wall - 0.14f}};
    const int n = (int)(sizeof(path) / sizeof(path[0]));
    kit_frame_pipe(kit, w, MAT_TRIM, path, n, DOWNPIPE_R, 10);

    const vec3 porch_joint = {PORCH_X0 + 0.5f, bottom, mid};
    kit_drip(kit, w, porch_joint, porch_joint, JOINT_DRIPS, FLOOR_Y);
    const vec3 room_joint = {front->openings[2].from + 0.5f, bottom, mid};
    kit_drip(kit, w, room_joint, room_joint, JOINT_DRIPS, 0.0f);
    const float window = 0.5f * (front->openings[1].from + front->openings[1].to);
    kit_drip(kit, w, (vec3){window - 0.15f, lip, lip_z - 0.005f},
             (vec3){window + 0.15f, lip, lip_z - 0.005f}, OVERFLOW_DRIPS, 0.0f);
    // Just past the shoe's mouth.
    const vec3 spout = {pipe_x, path[n - 1][1] - 0.02f, path[n - 1][2] - 0.02f};
    kit_drip(kit, w, spout, spout, SPOUT_DRIPS, 0.0f);
}

void house_build(Kit* kit) {
    // Exterior walls run from the ground to the ceiling slab's top, and the two
    // along X reach past the corners by half a thickness so the corners close.
    const float wall_top = CEIL_Y + 0.12f;
    const float corner = 0.5f * EXT_WALL;

    KitWall front = {
        .along_x = true,
        .at = HOUSE_FRONT_Z,
        .from = HOUSE_X0 - corner,
        .to = HOUSE_X1 + corner,
        .y0 = 0.0f,
        .y1 = wall_top,
        .thick = EXT_WALL,
        .inner = 1,
        .mat_inner = MAT_PLASTER,
        .mat_outer = MAT_SIDING,
        .openings = {{FRONT_DOOR_X0, FRONT_DOOR_X1, FLOOR_Y, DOOR_HEAD},
                     {KITCHEN_WIN_X0, KITCHEN_WIN_X1, KITCHEN_WIN_SILL, KITCHEN_WIN_HEAD},
                     {-4.1f, -2.5f, FLOOR_Y + 0.9f, FLOOR_Y + 2.1f}},
        .opening_count = 3};
    kit_wall(kit, &front);
    pane(kit, &front.openings[1]); // the kitchen's
    pane(kit, &front.openings[2]); // the front room's

    KitWall back = front;
    back.at = HOUSE_BACK_Z;
    back.inner = -1;
    back.opening_count = 0;
    kit_wall(kit, &back);

    KitWall west = {.along_x = false,
                    .at = HOUSE_X0,
                    .from = HOUSE_FRONT_Z,
                    .to = HOUSE_BACK_Z,
                    .y0 = 0.0f,
                    .y1 = wall_top,
                    .thick = EXT_WALL,
                    .inner = 1,
                    .mat_inner = MAT_PLASTER,
                    .mat_outer = MAT_SIDING};
    kit_wall(kit, &west);
    KitWall east = west;
    east.at = HOUSE_X1;
    east.inner = -1;
    kit_wall(kit, &east);

    // Interior walls, plaster both sides, floor to ceiling.
    KitWall hall_east = {.along_x = false,
                         .at = HALL_X1,
                         .from = HOUSE_FRONT_Z,
                         .to = HOUSE_BACK_Z,
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
    hall_west.opening_count = 0;
    kit_wall(kit, &hall_west);
    KitWall kitchen_back = {.along_x = true,
                            .at = KITCHEN_BACK_Z,
                            .from = HALL_X1,
                            .to = HOUSE_X1,
                            .y0 = FLOOR_Y,
                            .y1 = CEIL_Y,
                            .thick = INT_WALL,
                            .inner = -1,
                            .mat_inner = MAT_PLASTER,
                            .mat_outer = MAT_PLASTER};
    kit_wall(kit, &kitchen_back);

    // Floors, one slab per room so each takes its own surface: tile in the
    // kitchen, boards everywhere else.
    const KitFrame* w = &KIT_WORLD;
    kit_frame_box(kit, w, MAT_KITCHEN_FLOOR, HALL_X1, HOUSE_X1, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
                  KITCHEN_BACK_Z, true);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HALL_X1, HOUSE_X1, 0.0f, FLOOR_Y, KITCHEN_BACK_Z,
                  HOUSE_BACK_Z, true);
    kit_frame_box(kit, w, MAT_WOOD_FLOOR, HOUSE_X0, HALL_X1, 0.0f, FLOOR_Y, HOUSE_FRONT_Z,
                  HOUSE_BACK_Z, true);

    // One ceiling over everything, and the same pitched roof as the
    // neighbours', its frame on the front wall's outer face so the facade runs
    // along -x as the street sees it.
    kit_frame_box(kit, w, MAT_CEILING, HOUSE_X0, HOUSE_X1, CEIL_Y, CEIL_Y + 0.12f, HOUSE_FRONT_Z,
                  HOUSE_BACK_Z, true);
    const KitFrame roof = {{HOUSE_X1 + corner, 0.0f, HOUSE_FRONT_Z - corner}, GLM_PIf};
    const float overhang = 0.4f;
    const float tip_y =
        house_gable_roof(kit, &roof, HOUSE_X1 - HOUSE_X0 + EXT_WALL,
                         HOUSE_BACK_Z - HOUSE_FRONT_Z + EXT_WALL, wall_top, overhang, MAT_SIDING);
    gutter(kit, &front, tip_y, overhang);

    // The porch, level with the floor, and one step down to the yard.
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, FLOOR_Y, PORCH_Z0, HOUSE_FRONT_Z,
                  true);
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, 0.5f * FLOOR_Y, PORCH_Z0 - 0.32f,
                  PORCH_Z0, true);
}
