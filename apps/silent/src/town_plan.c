#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "hill.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "town_map.h"
#include "town_plan.h"
#include "woods.h"

// The grid the ground's cover and height are sampled on, at its cells' middles: wider than
// anything the map shows, the valley's far edge included.
#define GRID_X0   (-152.0f)
#define GRID_X1   156.0f
#define GRID_Z0   (-84.0f)
#define GRID_Z1   170.0f
#define GRID_STEP 2.0f

#define DRIVE_SAMPLES 200
#define TRACK_SAMPLES 160
#define SHORE_SAMPLES 180
#define LIP_STEP      1.0f // metres between the lip's samples

static void box(FILE* f, const char* kind, const char* name, float x0, float x1, float z0,
                float z1) {
    fprintf(f, "box %s %s %.3f %.3f %.3f %.3f\n", kind, name, x0, x1, z0, z1);
}

// `n` points along a curve to the end of its record: `at` takes 0..1 along an open one, and a
// bearing round a closed one, to its point.
static void points(FILE* f, int n, bool closed, void (*at)(float t, float* x, float* z)) {
    for (int k = 0; k < n; k++) {
        float x = 0.0f, z = 0.0f;
        at(closed ? 2.0f * GLM_PIf * (float)k / (float)n : (float)k / (float)(n - 1), &x, &z);
        fprintf(f, " %.3f %.3f", x, z);
    }
    fputc('\n', f);
}

static void plot(FILE* f, const char* kind, const char* side, int lot, const HousePlot* p) {
    if (street_lot_vacant(p))
        return;
    char name[16];
    snprintf(name, sizeof(name), "%s%d", side, lot);
    box(f, kind, name, p->x0, p->x1, p->z0, p->z1);
}

// What the ground at (x, z) is: c the chasm, v no ground at all past the world's edge, w the
// lake, t the woods, and . open ground.
static char cover_at(float x, float z) {
    if (x < VALLEY_X0 || z > VALLEY_Z1 || (x > VALLEY_X1 && z > WORLD_Z1))
        return 'v';
    if (land_lip_clearance(x, z, 0.0f) < 0.0f)
        return 'c';
    if (lake_shore_distance(x, z) < 0.0f)
        return 'w';
    return woods_cover(x, z) > 0.0f ? 't' : '.';
}

bool town_plan_write(const char* path, const StreetPlots* plots, unsigned int seed) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "silent: cannot write the town's plan to %s: %s\n", path, strerror(errno));
        return false;
    }
    fprintf(f, "# silent's town plan (spec 13.43), from silent --map-export, which "
               "tools/make_map.py draws the map from.\n");
    fprintf(f, "# Metres, x east and z south: box KIND NAME x0 x1 z0 z1, line NAME HALF COUNT x z "
               "..., poly NAME COUNT x z ..., place ID x z.\n");
    fprintf(f, "seed %u\n", seed);

    // The street, straight on to the chasm, and the cross street at the crossroads.
    box(f, "asphalt", "street", CROSS_X0, STREET_HALF_LEN, -ROAD_HALF_WIDTH, ROAD_HALF_WIDTH);
    box(f, "sidewalk", "street_n", CROSS_X0, STREET_HALF_LEN, -STREET_HALF_WIDTH, -ROAD_HALF_WIDTH);
    box(f, "sidewalk", "street_s", CROSS_X0, STREET_HALF_LEN, ROAD_HALF_WIDTH, STREET_HALF_WIDTH);
    box(f, "asphalt", "cross", CROSS_X - ROAD_HALF_WIDTH, CROSS_X + ROAD_HALF_WIDTH, CROSS_Z0,
        CROSS_Z1);
    box(f, "sidewalk", "cross_w", CROSS_X - STREET_HALF_WIDTH, CROSS_X - ROAD_HALF_WIDTH, CROSS_Z0,
        CROSS_Z1);
    box(f, "sidewalk", "cross_e", CROSS_X + ROAD_HALF_WIDTH, CROSS_X + STREET_HALF_WIDTH, CROSS_Z0,
        CROSS_Z1);

    // The lots either side, the terrace's wall, and the houses on them, ours among them.
    char name[16];
    for (int k = 0; k < NEAR_LOTS; k++) {
        snprintf(name, sizeof(name), "near%d", k);
        box(f, "lot", name, near_lot_line_x(k), near_lot_line_x(k + 1), STREET_HALF_WIDTH,
            BACK_FENCE_Z);
    }
    for (int k = 0; k < TERRACE_LOTS; k++) {
        snprintf(name, sizeof(name), "far%d", k);
        box(f, "lot", name, far_lot_line_x(k), far_lot_line_x(k + 1), TERRACE_BACK_Z,
            TERRACE_WALL_Z - TERRACE_WALL_THICK);
    }
    box(f, "wall", "terrace", -STREET_HALF_LEN, STREET_HALF_LEN,
        TERRACE_WALL_Z - TERRACE_WALL_THICK, TERRACE_WALL_Z);
    for (int k = 0; k < NEAR_LOTS; k++)
        plot(f, k == HOME_LOT ? "home" : "house", "near", k, &plots->near[k]);
    for (int k = 0; k < TERRACE_LOTS; k++)
        plot(f, "house", "far", k, &plots->far[k]);

    // Up the hill: the grounds, the mansion, and the graveyard by the drive.
    box(f, "grounds", "mansion", GROUNDS_X0, GROUNDS_X1, GROUNDS_Z0, GROUNDS_Z1);
    vec3 m0 = {0.0f, 0.0f, 0.0f}, m1 = {0.0f, 0.0f, 0.0f};
    mansion_at((vec3){HOUSE_OUT_X0, 0.0f, HOUSE_OUT_Z0}, m0);
    mansion_at((vec3){HOUSE_OUT_X1, 0.0f, HOUSE_OUT_Z1}, m1);
    box(f, "landmark", "mansion", m0[0], m1[0], m0[2], m1[2]);
    box(f, "graveyard", "graveyard", GRAVEYARD_X - GRAVEYARD_HX, GRAVEYARD_X + GRAVEYARD_HX,
        GRAVEYARD_Z - GRAVEYARD_HZ, GRAVEYARD_Z + GRAVEYARD_HZ);

    // Down by the lake: the cabin's pad and the cabin, which the map only shows once it is found.
    box(f, "pad", "cabin", CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1);
    box(f, "cabin", "cabin", CABIN_X0, CABIN_X1, CABIN_Z0, CABIN_Z1);

    // The drive and the track along their centre lines, the chasm's lip -- down the east side to
    // where it turns, then west along the ridge -- and the lake's shore.
    fprintf(f, "line drive %.3f %d", DRIVE_HALF, DRIVE_SAMPLES);
    points(f, DRIVE_SAMPLES, false, hill_drive_point);
    fprintf(f, "line track %.3f %d", TRACK_HALF, TRACK_SAMPLES);
    points(f, TRACK_SAMPLES, false, lake_track_point);
    const int east = (int)((RIDGE_Z - GRID_Z0) / LIP_STEP) + 1;
    const float turn = land_lip_x(RIDGE_Z);
    const int west = (int)((turn - GRID_X0) / LIP_STEP);
    fprintf(f, "line lip 0.000 %d", east + west);
    for (int k = 0; k < east; k++) {
        const float z = GRID_Z0 + LIP_STEP * (float)k;
        fprintf(f, " %.3f %.3f", land_lip_x(z), z);
    }
    for (int k = 1; k <= west; k++) {
        const float x = turn - LIP_STEP * (float)k;
        fprintf(f, " %.3f %.3f", x, land_ridge_lip_z(x));
    }
    fprintf(f, "\npoly shore %d", SHORE_SAMPLES);
    points(f, SHORE_SAMPLES, true, lake_shore_point);

    // What the player finds, and where each goes on the map.
    for (int i = 0; i < PLACE_COUNT; i++)
        fprintf(f, "place %s %.3f %.3f\n", PLACES[i].id, PLACES[i].mark[0], PLACES[i].mark[1]);

    // The ground's cover and height over the grid, a row a line from the north.
    const int cols = (int)((GRID_X1 - GRID_X0) / GRID_STEP);
    const int rows = (int)((GRID_Z1 - GRID_Z0) / GRID_STEP);
    fprintf(f, "grid %.3f %.3f %.3f %d %d\n", GRID_X0, GRID_Z0, GRID_STEP, cols, rows);
    for (int j = 0; j < rows; j++) {
        fprintf(f, "cover ");
        for (int i = 0; i < cols; i++)
            fputc(cover_at(GRID_X0 + GRID_STEP * ((float)i + 0.5f),
                           GRID_Z0 + GRID_STEP * ((float)j + 0.5f)),
                  f);
        fprintf(f, "\n");
    }
    for (int j = 0; j < rows; j++) {
        fprintf(f, "height");
        for (int i = 0; i < cols; i++)
            fprintf(f, " %.2f",
                    land_height(GRID_X0 + GRID_STEP * ((float)i + 0.5f),
                                GRID_Z0 + GRID_STEP * ((float)j + 0.5f)));
        fprintf(f, "\n");
    }
    const bool ok = ferror(f) == 0;
    if (fclose(f) != 0 || !ok) {
        fprintf(stderr, "silent: the town's plan at %s is incomplete\n", path);
        return false;
    }
    printf("silent: the town's plan written to %s\n", path);
    return true;
}
