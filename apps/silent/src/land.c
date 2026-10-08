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
 */

#define LAND_STEP  2.0f
#define LAND_X0    (-STREET_HALF_LEN)
#define NORTH_RISE 12.0f // metres the woods climb from the far lots' backs to the north edge
#define SOUTH_FALL 4.0f  // and fall from our back fences to the south edge
#define LAND_NOISE 0.6f  // metres of lumps in the woods
// Where the lots' height leaves the woods behind the east lot: it holds to here and is gone by
// the second, over this stretch north of the lots' backs.
#define BASE_FADE_X0 35.0f
#define BASE_FADE_Z  10.0f

int land_terrace_lot(float x) {
    const int lot = (int)floorf((x + STREET_HALF_LEN - 3.0f) / TERRACE_LOT_WIDTH);
    return lot < 0 ? 0 : (lot >= TERRACE_LOTS ? TERRACE_LOTS - 1 : lot);
}

void land_terrace_lot_span(int lot, float* x0, float* x1) {
    // The far houses stand TERRACE_LOT_WIDTH apart from x = -35, so a lot runs from midway to the
    // house west of it to midway to the one east, and the end lots to the street's ends.
    const float first = -STREET_HALF_LEN + 3.0f;
    *x0 = lot == 0 ? -STREET_HALF_LEN : first + TERRACE_LOT_WIDTH * (float)lot;
    *x1 = lot == TERRACE_LOTS - 1 ? STREET_HALF_LEN : first + TERRACE_LOT_WIDTH * (float)(lot + 1);
}

float land_terrace_height(float x) {
    const float t = (float)land_terrace_lot(x) / (float)(TERRACE_LOTS - 1);
    return TERRACE_HIGH + (TERRACE_LOW - TERRACE_HIGH) * t;
}

// How far (x, z) is outside the flat ground: the street's plate and the terrace together.
static float flat_distance(float x, float z) {
    const float dx = fmaxf(fmaxf(-STREET_HALF_LEN - x, x - STREET_HALF_LEN), 0.0f);
    const float dz = fmaxf(fmaxf(TERRACE_BACK_Z - z, z - BACK_FENCE_Z), 0.0f);
    return sqrtf(dx * dx + dz * dz);
}

static bool on_flat(float x, float z) {
    return x > -STREET_HALF_LEN && x < STREET_HALF_LEN && z > TERRACE_BACK_Z && z < BACK_FENCE_Z;
}

float land_height(float x, float z) {
    if (on_flat(x, z))
        return z < TERRACE_WALL_Z ? land_terrace_height(x) : 0.0f;

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
            p[0] = LAND_X0 + LAND_STEP * (float)i;
            p[2] = WORLD_Z0 + LAND_STEP * (float)j;
            p[1] = land_height(p[0], p[2]);
        }

    // Faceted, a flat normal a triangle, and the collider from the same cells -- less the
    // mansion's grounds, whose flat would be one long run of coplanar triangles and stand on a box
    // of hill_build's. The flat ground's cells are not the land's at all.
    const vec3 up = {0.0f, 1.0f, 0.0f};
    int n = 0;
    for (int j = 0; j < rows; j++)
        for (int i = 0; i < cols; i++) {
            const unsigned int a = (unsigned int)(j * (cols + 1) + i), b = a + 1;
            const unsigned int c = a + (unsigned int)(cols + 1), e = c + 1;
            const float mx = pos[3 * a] + 0.5f * LAND_STEP, mz = pos[3 * a + 2] + 0.5f * LAND_STEP;
            if (on_flat(mx, mz))
                continue;
            kit_tri_facing(kit, MAT_DIRT, &pos[3 * a], &pos[3 * c], &pos[3 * b], up);
            kit_tri_facing(kit, MAT_DIRT, &pos[3 * b], &pos[3 * c], &pos[3 * e], up);
            if (hill_on_grounds(mx, mz))
                continue;
            const unsigned int tri[6] = {a, c, b, b, c, e};
            for (int k = 0; k < 6; k++)
                idx[n++] = tri[k];
        }
    kit_mesh_collider(kit, pos, verts, idx, n);
    free(pos);
    free(idx);
}
