#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "hill.h"
#include "land.h"
#include "layout.h"
#include "mats.h"

/*
 * The ground under the whole world (spec 13.35), from one height function and over one grid.
 *
 * The street's plate -- road, sidewalks and our yards -- and the far side's terrace are flat and
 * stand on boxes, the street's and terrace.c's; the grid leaves their cells out, and its lines fall
 * on their edges, so a cell is either the land's or theirs. Everything else is land_height's sum:
 *
 *   - the hill past the street's east end, with the drive carved into it (hill_height);
 *   - behind the far lots, the woods climbing NORTH_RISE to the world's north edge from the lots'
 *     own height, which fades out toward the east end so the east end's retaining wall dies into
 *     the slope rather than running on forever;
 *   - behind our back fences, the woods falling SOUTH_FALL toward the lake valley;
 *   - lumps, none at the flat ground's edges, so the boxes and the grid meet exactly.
 *
 * The woods' terms die out past the street's east end, so the hill, the drive and everything
 * standing on them are where they were.
 *
 * West of the street the crossroads' plate is flat on its own boxes too (crossroads.c), and past
 * its barricaded arms the woods rise and fall as they do behind the lots, but leave the cross
 * street's line low, so it runs on into them as a cutting. The grid starts at the chasm: its first
 * column of vertices stands on the lip, wherever that has broken off, and across the road's end
 * that column's cells are left out, the road's broken end being crossroads.c's. From the lip the
 * ground goes on down as the chasm's face, to where the fog has it all.
 */

#define LAND_X0    CHASM_X // a whole number of steps west of the street's end, so lines still meet
#define NORTH_RISE 12.0f   // metres the woods climb from the far lots' backs to the north edge
#define SOUTH_FALL 4.0f    // and fall from our back fences to the south edge
#define LAND_NOISE 0.6f    // metres of lumps in the woods
// Where the lots' height leaves the woods behind the east lot: it holds to here and is gone by
// the second, over this stretch north of the lots' backs.
#define BASE_FADE_X0 35.0f
#define BASE_FADE_Z  10.0f
// The lip's face down, in rows this deep, to where the fog has it all.
#define CLIFF_STEP 3.0f
#define CLIFF_ROWS 15

float land_terrace_lot_height(int lot) {
    const float t = (float)lot / (float)(TERRACE_LOTS - 1);
    return TERRACE_HIGH + (TERRACE_LOW - TERRACE_HIGH) * t;
}

float land_terrace_height(float x) {
    const int lot = (int)floorf((x - LOT_GRID_X0) / TERRACE_LOT_WIDTH);
    return land_terrace_lot_height(lot < 0 ? 0 : (lot >= TERRACE_LOTS ? TERRACE_LOTS - 1 : lot));
}

float land_lip_x(float z) {
    const float ragged =
        CHASM_X - 0.25f + 1.75f * (0.6f * sinf(0.21f * z + 1.3f) + 0.4f * sinf(0.53f * z + 0.4f));
    // Straight across the road's end and its sidewalks, where the road broke off, a little past
    // the crossroads' ground so no cell of the grid's first column closes to nothing.
    const float road = STREET_HALF_WIDTH, straight = CROSS_X0 - 0.3f;
    return straight + (ragged - straight) * glm_smoothstep(road, road + 3.0f, fabsf(z));
}

// The flat ground, which stands on boxes of its own: the street's plate and the terrace together,
// and the crossroads'.
static const struct {
    float x0, x1, z0, z1;
} FLAT[] = {
    {-STREET_HALF_LEN, STREET_HALF_LEN, TERRACE_BACK_Z, BACK_FENCE_Z},
    {CROSS_X0, -STREET_HALF_LEN, CROSS_Z0, CROSS_Z1},
};

// How far (x, z) is outside the flat ground.
static float flat_distance(float x, float z) {
    float d = FLT_MAX;
    for (int i = 0; i < KIT_COUNT(FLAT); i++)
        d = fminf(d, plan_box_distance(x, z, FLAT[i].x0, FLAT[i].x1, FLAT[i].z0, FLAT[i].z1));
    return d;
}

// Whether the grid leaves (x, z) out: the flat ground, and across the road's end past it, where
// the road has gone over the edge.
static bool on_flat(float x, float z) {
    for (int i = 0; i < KIT_COUNT(FLAT); i++)
        if (x > FLAT[i].x0 && x < FLAT[i].x1 && z > FLAT[i].z0 && z < FLAT[i].z1)
            return true;
    return x < CROSS_X0 && fabsf(z) < STREET_HALF_WIDTH;
}

// East of the street's west end: the hill, the woods behind the lots, and their lumps.
static float east_height(float x, float z) {
    float h = hill_height(x, z);
    if (z < TERRACE_BACK_Z) {
        // The lots' own height carries on under the woods behind them, and fades behind the east
        // lot, where the terrace's east wall runs up into the slope until the two meet.
        float base = 0.0f;
        if (x <= STREET_HALF_LEN)
            base = land_terrace_height(x) *
                   (1.0f - glm_smoothstep(BASE_FADE_X0, STREET_HALF_LEN, x) *
                               glm_smoothstep(TERRACE_BACK_Z, TERRACE_BACK_Z - BASE_FADE_Z, z));
        h += base + NORTH_RISE * glm_smoothstep(TERRACE_BACK_Z, WORLD_Z0, z);
    }
    // The fall dies out past the street's east end, so the hill is as it was.
    if (z > BACK_FENCE_Z)
        h -= SOUTH_FALL * glm_smoothstep(BACK_FENCE_Z, WORLD_Z1, z) *
             (1.0f - glm_smoothstep(STREET_HALF_LEN - 5.0f, STREET_HALF_LEN + 15.0f, x));
    // The woods' lumps, short of the street's east end, where the hill's own take over.
    h += LAND_NOISE * hill_lumps(x, z) * glm_smoothstep(0.0f, 6.0f, flat_distance(x, z)) *
         (1.0f - glm_smoothstep(BASE_FADE_X0, STREET_HALF_LEN, x));
    return h;
}

/*
 * West of it: level round the crossroads, and past the barricades the same climb and fall as
 * behind the lots, but away from the cross street's line, which runs on into the woods low. Over
 * the last stretch before the street's west end it comes round to the east's own, so the two meet.
 */
static float west_height(float x, float z) {
    const float off_road = glm_smoothstep(8.0f, 18.0f, fabsf(x - CROSS_X));
    float h = NORTH_RISE * glm_smoothstep(CROSS_Z0, WORLD_Z0, z) * off_road -
              SOUTH_FALL * glm_smoothstep(CROSS_Z1, WORLD_Z1, z) * off_road;
    h += LAND_NOISE * hill_lumps(x, z) * glm_smoothstep(0.0f, 6.0f, flat_distance(x, z));
    const float east = east_height(-STREET_HALF_LEN, z);
    return h + (east - h) * glm_smoothstep(-STREET_HALF_LEN - 10.0f, -STREET_HALF_LEN, x);
}

float land_height(float x, float z) {
    if (on_flat(x, z))
        return z < TERRACE_WALL_Z && x > -STREET_HALF_LEN ? land_terrace_height(x) : 0.0f;
    return x < -STREET_HALF_LEN ? west_height(x, z) : east_height(x, z);
}

/*
 * The lip's face, down from the grid's first column of vertices -- or, across the road's end,
 * from under the road -- leaning out a little as it goes down so it shows from the top, and
 * broken up by a lumpy offset. A body along the lip, inside it, between the barricades.
 */
static void cliff(Kit* kit) {
    const float road = STREET_HALF_WIDTH;
    const int cols = (int)ceilf((WORLD_Z1 - WORLD_Z0) / LAND_STEP);
    const vec3 out = {-1.0f, 0.0f, 0.0f};
    vec3 prev[CLIFF_ROWS + 1];
    for (int j = 0; j <= cols; j++) {
        const float z = WORLD_Z0 + LAND_STEP * (float)j;
        const bool under_road = fabsf(z) < road - 0.01f;
        const float x_top = under_road ? CROSS_X0 : land_lip_x(z);
        const float y_top = under_road ? ROAD_Y - GROUND_DEPTH : land_height(x_top, z);
        vec3 col[CLIFF_ROWS + 1];
        for (int k = 0; k <= CLIFF_ROWS; k++) {
            const float depth = CLIFF_STEP * (float)k;
            // Buttresses and gullies down the face, and ledges across it, in two sizes.
            const float lump = k == 0 ? 0.0f
                                      : 2.2f * sinf(0.29f * z + 0.17f * depth) *
                                                cosf(0.13f * z - 0.23f * depth + 1.7f) +
                                            0.9f * sinf(1.1f * z + 0.9f * depth + 0.3f) *
                                                sinf(0.7f * depth - 0.4f * z);
            col[k][0] = x_top - 0.2f * depth + lump;
            col[k][1] = y_top - depth + (k == 0 ? 0.0f : 0.8f * sinf(0.9f * z + depth));
            col[k][2] = z + (k == 0 ? 0.0f : 0.6f * sinf(0.7f * depth + z));
        }
        if (j > 0)
            for (int k = 0; k < CLIFF_ROWS; k++) {
                kit_tri_facing(kit, MAT_CLIFF, prev[k], col[k], prev[k + 1], out);
                kit_tri_facing(kit, MAT_CLIFF, col[k], col[k + 1], prev[k + 1], out);
            }
        for (int k = 0; k <= CLIFF_ROWS; k++)
            glm_vec3_copy(col[k], prev[k]);
    }
    for (float z = CROSS_NORTH_Z; z < CROSS_SOUTH_Z; z += 2.0f) {
        const float zm = z + 1.0f;
        if (fabsf(zm) < road + 0.5f)
            continue;
        kit_collider(kit, (vec3){land_lip_x(zm) + 0.4f, 1.5f, zm}, (vec3){0.45f, 3.0f, 1.05f},
                     0.0f);
    }
}

void land_build(Kit* kit) {
    const int cols = (int)ceilf((WORLD_X1 - LAND_X0) / LAND_STEP);
    const int rows = (int)ceilf((WORLD_Z1 - WORLD_Z0) / LAND_STEP);
    const int verts = (cols + 1) * (rows + 1);
    float* pos = malloc(sizeof(float) * 3 * (size_t)verts);
    unsigned int* idx = malloc(sizeof(unsigned int) * 6 * (size_t)cols * (size_t)rows);
    if (!pos || !idx) {
        free(pos);
        free(idx);
        return;
    }
    for (int j = 0; j <= rows; j++)
        for (int i = 0; i <= cols; i++) {
            float* p = &pos[3 * (j * (cols + 1) + i)];
            p[2] = WORLD_Z0 + LAND_STEP * (float)j;
            p[0] = i == 0 ? land_lip_x(p[2]) : LAND_X0 + LAND_STEP * (float)i;
            p[1] = land_height(p[0], p[2]);
        }

    // Faceted, a flat normal a triangle, and the collider from the same cells -- less the
    // mansion's grounds, whose flat would be one long run of coplanar triangles and stand on a box
    // of hill_build's. The flat ground's cells are not the land's at all. Under the woods the
    // ground is their floor, the cells at its east edge taking it by a hash so the edge is ragged.
    const vec3 up = {0.0f, 1.0f, 0.0f};
    int n = 0;
    for (int j = 0; j < rows; j++)
        for (int i = 0; i < cols; i++) {
            const unsigned int a = (unsigned int)(j * (cols + 1) + i), b = a + 1;
            const unsigned int c = a + (unsigned int)(cols + 1), e = c + 1;
            const float mx = pos[3 * a] + 0.5f * LAND_STEP, mz = pos[3 * a + 2] + 0.5f * LAND_STEP;
            if (on_flat(mx, mz))
                continue;
            const unsigned int h = ((unsigned int)i * 73856093u) ^ ((unsigned int)j * 19349663u);
            const float ragged = (float)(h % 1000u) / 1000.0f * 8.0f;
            const bool woods =
                (mz < TERRACE_BACK_Z || mz > BACK_FENCE_Z || mx < -STREET_HALF_LEN) &&
                mx < WOODS_EAST_X - 4.0f + ragged;
            const int mat = woods ? MAT_WOODS_FLOOR : MAT_DIRT;
            kit_tri_facing(kit, mat, &pos[3 * a], &pos[3 * c], &pos[3 * b], up);
            kit_tri_facing(kit, mat, &pos[3 * b], &pos[3 * c], &pos[3 * e], up);
            if (hill_on_grounds(mx, mz))
                continue;
            const unsigned int tri[6] = {a, c, b, b, c, e};
            for (int k = 0; k < 6; k++)
                idx[n++] = tri[k];
        }
    kit_mesh_collider(kit, pos, verts, idx, n);
    free(pos);
    free(idx);
    cliff(kit);
}
