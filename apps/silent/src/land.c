#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "hill.h"
#include "lake.h"
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
 * its north arm's barricade the woods rise as they do behind the lots, but leave the cross
 * street's line low, so it runs on into them as a cutting; past its south arm they fall toward
 * the lake valley (spec 13.41), whose ground lake.c carves into all of this last. The chasm cuts
 * the grid: one column of vertices stands on its east lip, wherever that has broken off, as far
 * south as RIDGE_Z, where the lip turns west and one row stands on its south lip; the cells north
 * and west of those are left out, and so are those across the road's end, the road's broken end
 * being crossroads.c's. From the lip the ground goes on down as the chasm's face, to where the fog
 * has it all.
 */

#define LAND_X0 VALLEY_X0 // a whole number of steps west of the street's end, so lines still meet
#define NORTH_RISE 12.0f  // metres the woods climb from the far lots' backs to the north edge
#define SOUTH_FALL 4.0f   // and fall from our back fences to the south edge
#define FALL_Z1    85.0f  // where that fall is whole: the world's south edge before the valley
#define LAND_NOISE 0.6f   // metres of lumps in the woods
// The grid's column on the chasm's east lip, and its row on the south lip.
#define I_LIP   ((int)((CHASM_X - LAND_X0) / LAND_STEP))
#define J_RIDGE ((int)((RIDGE_Z - WORLD_Z0) / LAND_STEP))
// And the corner of the world past the valley's east edge, south of the woods behind our side,
// where there is no ground.
#define I_VALLEY_EAST ((int)((VALLEY_X1 - LAND_X0) / LAND_STEP))
#define J_VALLEY      ((int)ceilf((WORLD_Z1 - WORLD_Z0) / LAND_STEP))
// Steeper than this, the valley's ground is the rock showing through.
#define ROCK_SLOPE_COS 0.8387f // cos 33 degrees
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

// A lip's broken edge about `line`, `t` metres along it, from two waves at their own phases.
static float broken_edge(float line, float t, float phase_a, float phase_b) {
    return line - 0.25f +
           1.75f * (0.6f * sinf(0.21f * t + phase_a) + 0.4f * sinf(0.53f * t + phase_b));
}

float land_lip_x(float z) {
    const float edge = broken_edge(CHASM_X, z, 1.3f, 0.4f);
    // Straight across the road's end and its sidewalks, where the road broke off, a little past
    // the crossroads' ground so no cell of the grid's first column closes to nothing.
    const float road = STREET_HALF_WIDTH, straight = CROSS_X0 - 0.3f;
    return straight + (edge - straight) * glm_smoothstep(road, road + 3.0f, fabsf(z));
}

float land_ridge_lip_z(float x) {
    const float edge = broken_edge(RIDGE_Z, x, 2.1f, 0.9f);
    // Square to the east lip at their corner, so the two meet.
    const float corner = land_lip_x(RIDGE_Z);
    return RIDGE_Z + (edge - RIDGE_Z) * glm_smoothstep(0.0f, 4.0f, corner - x);
}

float land_lip_clearance(float x, float z, float clear) {
    // West of the corner the ridge's lip, north of it the chasm; east of it the east lip, west of
    // it the chasm as far south as the corner.
    if (x < land_lip_x(RIDGE_Z))
        return z - (land_ridge_lip_z(x) + clear);
    return z < RIDGE_Z ? x - (land_lip_x(z) + clear) : FLT_MAX;
}

// The flat ground, which stands on boxes of its own, each at its level: the street's plate and the
// terrace together, whose lots step up behind the terrace's wall, the crossroads', and the cabin's
// pad down by the lake.
static const struct {
    float x0, x1, z0, z1, y;
} FLAT[] = {
    {-STREET_HALF_LEN, STREET_HALF_LEN, TERRACE_BACK_Z, BACK_FENCE_Z, 0.0f},
    {CROSS_X0, -STREET_HALF_LEN, CROSS_Z0, CROSS_Z1, 0.0f},
    {CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1, CABIN_PAD_Y},
};

// How far (x, z) is outside the flat ground.
static float flat_distance(float x, float z) {
    float d = FLT_MAX;
    for (int i = 0; i < KIT_COUNT(FLAT); i++)
        d = fminf(d, plan_box_distance(x, z, FLAT[i].x0, FLAT[i].x1, FLAT[i].z0, FLAT[i].z1));
    return d;
}

// Which of FLAT's boxes (x, z) is on, or -1.
static int flat_at(float x, float z) {
    for (int i = 0; i < KIT_COUNT(FLAT); i++)
        if (x > FLAT[i].x0 && x < FLAT[i].x1 && z > FLAT[i].z0 && z < FLAT[i].z1)
            return i;
    return -1;
}

// Across the road's end past the crossroads, where the road has gone over the edge.
static bool past_road_end(float x, float z) {
    return x < CROSS_X0 && fabsf(z) < STREET_HALF_WIDTH;
}

// Whether the grid leaves (x, z) out: the flat ground, and past the road's end.
static bool on_flat(float x, float z) {
    return flat_at(x, z) >= 0 || past_road_end(x, z);
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
        h -= SOUTH_FALL * glm_smoothstep(BACK_FENCE_Z, FALL_Z1, z) *
             (1.0f - glm_smoothstep(STREET_HALF_LEN - 5.0f, STREET_HALF_LEN + 15.0f, x));
    // The woods' lumps, short of the street's east end, where the hill's own take over.
    h += LAND_NOISE * hill_lumps(x, z) * glm_smoothstep(0.0f, 6.0f, flat_distance(x, z)) *
         (1.0f - glm_smoothstep(BASE_FADE_X0, STREET_HALF_LEN, x));
    return h;
}

/*
 * West of it: level round the crossroads, and past the barricades the same climb and fall as
 * behind the lots. The climb leaves the cross street's line, which runs on north into the woods
 * low; the fall takes it, since the lake track runs down it and would otherwise sit in a trench
 * (spec 13.41). Over the last stretch before the street's west end it comes round to the east's
 * own, so the two meet.
 */
static float west_height(float x, float z) {
    const float off_road = glm_smoothstep(8.0f, 18.0f, fabsf(x - CROSS_X));
    float h = NORTH_RISE * glm_smoothstep(CROSS_Z0, WORLD_Z0, z) * off_road -
              SOUTH_FALL * glm_smoothstep(CROSS_Z1, FALL_Z1, z);
    h += LAND_NOISE * hill_lumps(x, z) * glm_smoothstep(0.0f, 6.0f, flat_distance(x, z));
    const float meet = glm_smoothstep(-STREET_HALF_LEN - 10.0f, -STREET_HALF_LEN, x);
    if (meet <= 0.0f)
        return h;
    const float east = east_height(-STREET_HALF_LEN, z);
    return h + (east - h) * meet;
}

float land_height(float x, float z) {
    const int flat = flat_at(x, z);
    if (flat >= 0)
        return z < TERRACE_WALL_Z && x > -STREET_HALF_LEN ? land_terrace_height(x) : FLAT[flat].y;
    if (past_road_end(x, z))
        return 0.0f;
    return lake_ground(x, z, x < -STREET_HALF_LEN ? west_height(x, z) : east_height(x, z));
}

/*
 * One column of the lip's face, down from `top`, `u` metres along the lip: leaning out over the
 * chasm a little as it goes down so it shows from the top, and broken up by a lumpy offset. It
 * leans out along `axis`, x on the east lip and z on the ridge's. `f` takes the lean and the lumps
 * down to nothing where the two meet, so their columns there go straight down and never cross.
 */
static void face_column(const vec3 top, float u, int axis, float f, vec3 col[CLIFF_ROWS + 1]) {
    const float lean = 0.2f * f, wobble = 0.6f * f;
    const float out = top[axis], side = top[2 - axis];
    for (int k = 0; k <= CLIFF_ROWS; k++) {
        const float depth = CLIFF_STEP * (float)k;
        // Buttresses and gullies down the face, and ledges across it, in two sizes.
        const float lump =
            k == 0
                ? 0.0f
                : 2.2f * sinf(0.29f * u + 0.17f * depth) * cosf(0.13f * u - 0.23f * depth + 1.7f) +
                      0.9f * sinf(1.1f * u + 0.9f * depth + 0.3f) * sinf(0.7f * depth - 0.4f * u);
        const float o = out - lean * depth + lump * f;
        const float s = side + (k == 0 ? 0.0f : wobble * sinf(0.7f * depth + u));
        col[k][axis] = o;
        col[k][1] = top[1] - depth + (k == 0 ? 0.0f : 0.8f * sinf(0.9f * u + depth));
        col[k][2 - axis] = s;
    }
}

/*
 * The lip's face, down from the grid's column of vertices on the east lip -- or, across the
 * road's end, from under the road -- to the corner where the lip turns west at RIDGE_Z, and on
 * from there along the ridge's lip off the grid's west edge. Bodies along the lip, inside it,
 * wherever anyone can walk to it: between the barricades, and from the south one on down the
 * valley side, round the corner and along the ridge.
 */
static void cliff(Kit* kit) {
    const float road = STREET_HALF_WIDTH;
    const float corner_x = land_lip_x(RIDGE_Z);
    vec3 prev[CLIFF_ROWS + 1];
    vec3 prev_out = {0.0f, 0.0f, 0.0f};
    // The east lip, z up to the corner, then the ridge's lip, x west from it.
    for (int j = 0; j <= J_RIDGE + I_LIP; j++) {
        const bool east = j <= J_RIDGE;
        vec3 top, out;
        float u;
        if (east) {
            const float z = WORLD_Z0 + LAND_STEP * (float)j;
            const bool under_road = fabsf(z) < road - 0.01f;
            top[0] = under_road ? CROSS_X0 : land_lip_x(z);
            top[1] = under_road ? ROAD_Y - GROUND_DEPTH : land_height(top[0], z);
            top[2] = z;
            u = z;
            glm_vec3_copy((vec3){-1.0f, 0.0f, 0.0f}, out);
        } else {
            const float x = CHASM_X - LAND_STEP * (float)(j - J_RIDGE);
            top[0] = x;
            top[2] = land_ridge_lip_z(x);
            top[1] = land_height(x, top[2]);
            u = RIDGE_Z + (corner_x - x);
            glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, out);
        }
        const float f = glm_smoothstep(0.0f, 12.0f, fabsf(u - RIDGE_Z));
        vec3 col[CLIFF_ROWS + 1];
        face_column(top, u, east ? 0 : 2, f, col);
        if (j > 0) {
            // Each strip faces out of the chasm's side it is on; the one round the corner, both.
            vec3 facing;
            glm_vec3_add(out, prev_out, facing);
            for (int k = 0; k < CLIFF_ROWS; k++) {
                kit_tri_facing(kit, MAT_CLIFF, prev[k], col[k], prev[k + 1], facing);
                kit_tri_facing(kit, MAT_CLIFF, col[k], col[k + 1], prev[k + 1], facing);
            }
        }
        for (int k = 0; k <= CLIFF_ROWS; k++)
            glm_vec3_copy(col[k], prev[k]);
        glm_vec3_copy(out, prev_out);
    }
    for (float z = CROSS_NORTH_Z; z < CROSS_SOUTH_Z; z += 2.0f) {
        const float zm = z + 1.0f;
        if (fabsf(zm) < road + 0.5f)
            continue;
        kit_collider(kit, (vec3){land_lip_x(zm) + 0.4f, 1.5f, zm}, (vec3){0.45f, 3.0f, 1.05f},
                     0.0f);
    }
    // Down the valley side to the corner, and along the ridge to the valley's west wall.
    for (float z = CROSS_SOUTH_Z; z < RIDGE_Z; z += 2.0f) {
        const float zm = z + 1.0f, x = land_lip_x(zm) + 0.4f;
        kit_collider(kit, (vec3){x, land_height(x, zm) + 1.5f, zm}, (vec3){0.45f, 3.0f, 1.05f},
                     0.0f);
    }
    kit_collider(kit,
                 (vec3){corner_x + 0.4f, land_height(corner_x + 0.4f, RIDGE_Z + 0.4f) + 1.5f,
                        RIDGE_Z + 0.4f},
                 (vec3){0.6f, 3.0f, 0.6f}, 0.0f);
    for (float x = corner_x - 1.0f; x > VALLEY_WALL_X0; x -= 2.0f) {
        const float xm = x - 1.0f, z = land_ridge_lip_z(xm) + 0.4f;
        kit_collider(kit, (vec3){xm, land_height(xm, z) + 1.5f, z}, (vec3){1.05f, 3.0f, 0.45f},
                     0.0f);
    }
}

// Whether the grid has no ground in cell (i, j): the chasm north and west of its lips, and the
// corner of the world past the valley's east edge.
static bool cell_missing(int i, int j) {
    return (i < I_LIP && j < J_RIDGE) || (i >= I_VALLEY_EAST && j >= J_VALLEY);
}

// Whether triangle a, b, c is too steep to hold soil.
static bool too_steep(const float* a, const float* b, const float* c) {
    vec3 ab, ac, n;
    glm_vec3_sub((float*)b, (float*)a, ab);
    glm_vec3_sub((float*)c, (float*)a, ac);
    glm_vec3_cross(ab, ac, n);
    return fabsf(n[1]) < ROCK_SLOPE_COS * glm_vec3_norm(n);
}

// The valley's ground, by what lies on it: mud along the water, rock where it is too steep to hold
// soil -- each only in the valley, so no cell the town had changes -- and otherwise the woods'.
static int valley_mat(const float* a, const float* b, const float* c, int woods_mat) {
    const float mz = (a[2] + b[2] + c[2]) / 3.0f, mx = (a[0] + b[0] + c[0]) / 3.0f;
    if (mz <= CROSS_Z1 || mx >= -STREET_HALF_LEN)
        return woods_mat;
    if (fminf(fminf(a[1], b[1]), c[1]) < LAKE_Y + 0.3f)
        return MAT_SHORE;
    return too_steep(a, b, c) ? MAT_CLIFF : woods_mat;
}

// The grid's vertex (i, j): on the east lip as far as the corner, and west of it on the ridge's.
static void grid_vertex(int i, int j, float* p) {
    p[0] = LAND_X0 + LAND_STEP * (float)i;
    p[2] = WORLD_Z0 + LAND_STEP * (float)j;
    if (i == I_LIP && j <= J_RIDGE)
        p[0] = land_lip_x(p[2]);
    else if (j == J_RIDGE && i < I_LIP)
        p[2] = land_ridge_lip_z(p[0]);
    p[1] = land_height(p[0], p[2]);
}

bool land_too_steep(float x, float z) {
    const float gi = (x - LAND_X0) / LAND_STEP, gj = (z - WORLD_Z0) / LAND_STEP;
    const int i = (int)floorf(gi), j = (int)floorf(gj);
    vec3 a = {0.0f}, b = {0.0f}, c = {0.0f}, e = {0.0f};
    grid_vertex(i, j, a);
    grid_vertex(i + 1, j, b);
    grid_vertex(i, j + 1, c);
    grid_vertex(i + 1, j + 1, e);
    // The cell's two triangles, as land_build lays them, meet along b-c.
    return gi - (float)i + gj - (float)j < 1.0f ? too_steep(a, c, b) : too_steep(b, c, e);
}

void land_build(Kit* kit) {
    const int cols = (int)ceilf((WORLD_X1 - LAND_X0) / LAND_STEP);
    const int rows = (int)ceilf((VALLEY_Z1 - WORLD_Z0) / LAND_STEP);
    const int verts = (cols + 1) * (rows + 1);
    float* pos = malloc(sizeof(float) * 3 * (size_t)verts);
    unsigned int* idx = malloc(sizeof(unsigned int) * 6 * (size_t)cols * (size_t)rows);
    if (!pos || !idx) {
        free(pos);
        free(idx);
        return;
    }
    for (int j = 0; j <= rows; j++)
        for (int i = 0; i <= cols; i++)
            grid_vertex(i, j, &pos[3 * (j * (cols + 1) + i)]);

    // Faceted, a flat normal a triangle, and the collider from the same cells -- less the
    // mansion's grounds, whose flat would be one long run of coplanar triangles and stand on a box
    // of hill_build's. The lake's bed collides all the way out: whatever keeps a wader out of the
    // deep water, nobody falls through the bed if they get past it. The flat ground's cells are
    // not the land's at all. Under the woods the ground is their floor, the cells at its
    // east edge taking it by a hash so the edge is ragged -- a hash of the cell's place east of the
    // lip, as it was before the grid reached west of it.
    const vec3 up = {0.0f, 1.0f, 0.0f};
    int n = 0;
    for (int j = 0; j < rows; j++)
        for (int i = 0; i < cols; i++) {
            if (cell_missing(i, j))
                continue;
            const unsigned int a = (unsigned int)(j * (cols + 1) + i), b = a + 1;
            const unsigned int c = a + (unsigned int)(cols + 1), e = c + 1;
            const float mx = pos[3 * a] + 0.5f * LAND_STEP, mz = pos[3 * a + 2] + 0.5f * LAND_STEP;
            if (on_flat(mx, mz))
                continue;
            const unsigned int h =
                ((unsigned int)(i - I_LIP) * 73856093u) ^ ((unsigned int)j * 19349663u);
            const float ragged = (float)(h % 1000u) / 1000.0f * 8.0f;
            const bool woods =
                (mz < TERRACE_BACK_Z || mz > BACK_FENCE_Z || mx < -STREET_HALF_LEN) &&
                mx < WOODS_EAST_X - 4.0f + ragged;
            const int mat = woods ? MAT_WOODS_FLOOR : MAT_DIRT;
            const float *pa = &pos[3 * a], *pb = &pos[3 * b], *pc = &pos[3 * c], *pe = &pos[3 * e];
            kit_tri_facing(kit, valley_mat(pa, pc, pb, mat), pa, pc, pb, up);
            kit_tri_facing(kit, valley_mat(pb, pc, pe, mat), pb, pc, pe, up);
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
