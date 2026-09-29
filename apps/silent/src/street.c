#include "layout.h"
#include "mats.h"
#include "street.h"

// How far the yards reach behind the houses on either side. Past this is fog.
#define YARD_DEPTH   40.0f
#define GROUND_DEPTH 0.4f // how thick the ground boxes are, below their tops

static void ground(Kit* kit, int mat, float z0, float z1, float top) {
    const float y0 = top - GROUND_DEPTH;
    kit_box(kit, mat, (vec3){0.0f, 0.5f * (y0 + top), 0.5f * (z0 + z1)},
            (vec3){STREET_HALF_LEN, 0.5f * (top - y0), 0.5f * (z1 - z0)}, 0.0f, true);
}

void street_build(Kit* kit) {
    const float kerb = ROAD_HALF_WIDTH;
    const float walk = ROAD_HALF_WIDTH + SIDEWALK_WIDTH;
    ground(kit, MAT_ASPHALT, -kerb, kerb, ROAD_Y);
    ground(kit, MAT_CONCRETE, kerb, walk, 0.0f);
    ground(kit, MAT_CONCRETE, -walk, -kerb, 0.0f);
    ground(kit, MAT_DIRT, walk, walk + YARD_DEPTH, 0.0f);
    ground(kit, MAT_DIRT, -walk - YARD_DEPTH, -walk, 0.0f);

    // The world's edge: walls the fog hides, so a walk down the street ends in
    // grey rather than off the end of the ground.
    const float h = 3.0f;
    const float zmax = walk + YARD_DEPTH;
    kit_collider(kit, (vec3){-STREET_HALF_LEN + 1.0f, h, 0.0f}, (vec3){0.5f, h, zmax}, 0.0f);
    kit_collider(kit, (vec3){STREET_HALF_LEN - 1.0f, h, 0.0f}, (vec3){0.5f, h, zmax}, 0.0f);
    kit_collider(kit, (vec3){0.0f, h, zmax - 1.0f}, (vec3){STREET_HALF_LEN, h, 0.5f}, 0.0f);
    kit_collider(kit, (vec3){0.0f, h, -zmax + 1.0f}, (vec3){STREET_HALF_LEN, h, 0.5f}, 0.0f);
}
