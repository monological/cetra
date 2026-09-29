#include "house.h"
#include "houses.h"
#include "layout.h"
#include "mats.h"

// Floor and ceiling slabs, one per room so each takes its own surface.
static void slab(Kit* kit, int mat, float x0, float x1, float z0, float z1, float y0, float y1) {
    kit_box(kit, mat, (vec3){0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1)},
            (vec3){0.5f * (x1 - x0), 0.5f * (y1 - y0), 0.5f * (z1 - z0)}, 0.0f, true);
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
    // Thin grimy panes in the middle of the wall, and a body through the whole
    // thickness so nobody climbs through.
    const float kx = 0.5f * (KITCHEN_WIN_X0 + KITCHEN_WIN_X1);
    const float ky = 0.5f * (KITCHEN_WIN_SILL + KITCHEN_WIN_HEAD);
    const vec3 kh = {0.5f * (KITCHEN_WIN_X1 - KITCHEN_WIN_X0),
                     0.5f * (KITCHEN_WIN_HEAD - KITCHEN_WIN_SILL), 0.003f};
    kit_box(kit, MAT_WINDOW_GLASS, (vec3){kx, ky, HOUSE_FRONT_Z}, kh, 0.0f, false);
    kit_collider(kit, (vec3){kx, ky, HOUSE_FRONT_Z}, (vec3){kh[0], kh[1], 0.5f * EXT_WALL}, 0.0f);
    kit_box(kit, MAT_WINDOW_GLASS, (vec3){-3.3f, FLOOR_Y + 1.5f, HOUSE_FRONT_Z},
            (vec3){0.8f, 0.6f, 0.003f}, 0.0f, false);
    kit_collider(kit, (vec3){-3.3f, FLOOR_Y + 1.5f, HOUSE_FRONT_Z},
                 (vec3){0.8f, 0.6f, 0.5f * EXT_WALL}, 0.0f);

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

    // Floors: tile in the kitchen, boards everywhere else.
    slab(kit, MAT_KITCHEN_FLOOR, HALL_X1, HOUSE_X1, HOUSE_FRONT_Z, KITCHEN_BACK_Z, 0.0f, FLOOR_Y);
    slab(kit, MAT_WOOD_FLOOR, HALL_X1, HOUSE_X1, KITCHEN_BACK_Z, HOUSE_BACK_Z, 0.0f, FLOOR_Y);
    slab(kit, MAT_WOOD_FLOOR, HOUSE_X0, HALL_X1, HOUSE_FRONT_Z, HOUSE_BACK_Z, 0.0f, FLOOR_Y);

    // One ceiling over everything, and the same pitched roof as the
    // neighbours', its frame on the front wall's outer face so the facade runs
    // along -x as the street sees it.
    slab(kit, MAT_CEILING, HOUSE_X0, HOUSE_X1, HOUSE_FRONT_Z, HOUSE_BACK_Z, CEIL_Y, CEIL_Y + 0.12f);
    const KitFrame roof = {{HOUSE_X1 + corner, 0.0f, HOUSE_FRONT_Z - corner}, GLM_PIf};
    const float depth = HOUSE_BACK_Z - HOUSE_FRONT_Z + EXT_WALL;
    house_gable_roof(kit, &roof, HOUSE_X1 - HOUSE_X0 + EXT_WALL, depth, wall_top, 0.36f * depth,
                     0.4f, MAT_SIDING);

    // The porch, level with the floor, and one step down to the yard.
    slab(kit, MAT_PORCH, PORCH_X0, PORCH_X1, PORCH_Z0, HOUSE_FRONT_Z, 0.0f, FLOOR_Y);
    slab(kit, MAT_PORCH, PORCH_X0, PORCH_X1, PORCH_Z0 - 0.32f, PORCH_Z0, 0.0f, 0.5f * FLOOR_Y);
}
