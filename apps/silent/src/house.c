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

// The gutter's reach along the front eave, the downpipe's place on the facade, and the leaks,
// as drops a second at the reference rain: a joint seeps, the lip over the kitchen window
// overflows where the gutter sags, and the shoe at the pipe's foot runs nearly in a stream.
#define GUTTER_HALF    5.45f
#define DOWNPIPE_X     4.75f
#define DOWNPIPE_R     0.035f
#define JOINT_DRIPS    3.0f
#define OVERFLOW_DRIPS 15.0f
#define SPOUT_DRIPS    40.0f

/*
 * A gutter hung on the fascia under the front eave's edge at (`tip_y`, `fascia_z`): a channel
 * open at the top, its back the fascia, and a downpipe that drops to the wall and down it to
 * a shoe kicked out over the yard.
 */
static void gutter(Kit* kit, Drips* drips, float tip_y, float fascia_z) {
    const KitFrame* w = &KIT_WORLD;
    const float bottom = tip_y - 0.14f, lip = tip_y - 0.03f, front = fascia_z - 0.13f;
    kit_frame_box(kit, w, MAT_TRIM, -GUTTER_HALF, GUTTER_HALF, bottom, bottom + 0.015f, front,
                  fascia_z, false);
    kit_frame_box(kit, w, MAT_TRIM, -GUTTER_HALF, GUTTER_HALF, bottom, lip, front, front + 0.015f,
                  false);
    for (int side = -1; side <= 1; side += 2)
        kit_frame_box(kit, w, MAT_TRIM, side * GUTTER_HALF - 0.0075f, side * GUTTER_HALF + 0.0075f,
                      bottom, lip, front, fascia_z, false);

    // Out of the gutter's floor, back to the wall in a swan neck, and down it.
    const float mid = 0.5f * (front + fascia_z);
    const float wall = HOUSE_FRONT_Z - 0.5f * EXT_WALL - DOWNPIPE_R - 0.015f;
    const vec3 path[] = {{DOWNPIPE_X, bottom, mid},
                         {DOWNPIPE_X, bottom - 0.15f, mid},
                         {DOWNPIPE_X, bottom - 0.27f, wall},
                         {DOWNPIPE_X, 0.3f, wall},
                         {DOWNPIPE_X, 0.14f, wall - 0.14f}};
    kit_frame_pipe(kit, w, MAT_TRIM, path, (int)(sizeof(path) / sizeof(path[0])), DOWNPIPE_R, 10);

    // The joints leak from the gutter's floor: one over the porch by the door, one past the
    // front room's window.
    drips_add(drips, w, (vec3){-1.9f, bottom, mid}, (vec3){-1.9f, bottom, mid}, JOINT_DRIPS,
              FLOOR_Y);
    drips_add(drips, w, (vec3){-3.6f, bottom, mid}, (vec3){-3.6f, bottom, mid}, JOINT_DRIPS, 0.0f);
    const float window = 0.5f * (KITCHEN_WIN_X0 + KITCHEN_WIN_X1);
    drips_add(drips, w, (vec3){window - 0.15f, lip, front - 0.005f},
              (vec3){window + 0.15f, lip, front - 0.005f}, OVERFLOW_DRIPS, 0.0f);
    drips_add(drips, w, (vec3){DOWNPIPE_X, 0.12f, wall - 0.16f},
              (vec3){DOWNPIPE_X, 0.12f, wall - 0.16f}, SPOUT_DRIPS, 0.0f);
}

void house_build(Kit* kit, Drips* drips) {
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
    house_gable_roof(kit, &roof, HOUSE_X1 - HOUSE_X0 + EXT_WALL,
                     HOUSE_BACK_Z - HOUSE_FRONT_Z + EXT_WALL, wall_top, overhang, MAT_SIDING);
    gutter(kit, drips, house_eave_tip_y(wall_top, overhang), HOUSE_FRONT_Z - corner - overhang);

    // The porch, level with the floor, and one step down to the yard.
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, FLOOR_Y, PORCH_Z0, HOUSE_FRONT_Z,
                  true);
    kit_frame_box(kit, w, MAT_PORCH, PORCH_X0, PORCH_X1, 0.0f, 0.5f * FLOOR_Y, PORCH_Z0 - 0.32f,
                  PORCH_Z0, true);
}
