#include <float.h>
#include <math.h>
#include <stdio.h>

#include "cetra/loading_screen.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/procedural/rock.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "hill.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "woods.h"

/*
 * The woods (spec 13.35). A jittered grid of sites over the two bands -- behind the far side's
 * back fences, and behind ours -- each a conifer from one of a handful of models grown once by the
 * engine's generator, or now and then one of the dead trees. Thin along the fences, so the first
 * trunks stand clear of them, and closing up behind.
 *
 * Every tree is hung once, under its model's group so a model's copies are adjacent and draw
 * together, and never taken down. Hanging only those within the fog's reach of the eye, and
 * taking the rest down as the eye moved, changed the scene graph on nearly every frame of a walk,
 * and a changed graph draws every kept face of every cached light again: frames of 650 ms, a few
 * a second, whenever the player moved. What keeps the far trees cheap instead is their levels of
 * detail -- the wood by the engine's simplifier, the sprays by thinning them -- each model's meshes
 * carrying their own LOD scale, so the engine's bias stays the GUI's to move.
 */

// The conifers' models: five spruces, two firs and a snag.
static const struct {
    TreePreset preset;
    int seed;
} CONIFERS[] = {
    {TREE_PRESET_SPRUCE, 1}, {TREE_PRESET_SPRUCE, 2}, {TREE_PRESET_SPRUCE, 3},
    {TREE_PRESET_SPRUCE, 7}, {TREE_PRESET_SPRUCE, 8}, {TREE_PRESET_FIR, 2},
    {TREE_PRESET_FIR, 3},    {TREE_PRESET_SNAG, 1},
};
#define CONIFER_MODELS KIT_COUNT(CONIFERS)
#define SNAG_MODEL     (CONIFER_MODELS - 1)

/*
 * What stands at a site: a dead tree, the snag, or one of the live conifers, each its share of the
 * sites and spread evenly over its models, from `first`.
 */
#define DEAD_SHARE 0.10f
#define SNAG_SHARE 0.06f
typedef struct Species {
    float share;
    bool dead;
    int first, models;
} Species;
static const Species SPECIES[] = {
    {DEAD_SHARE, true, 0, TREE_MODELS},
    {SNAG_SHARE, false, SNAG_MODEL, 1},
    {1.0f - DEAD_SHARE - SNAG_SHARE, false, 0, SNAG_MODEL},
};

// The engine's ladder is set for a mesh the size of a room: at 1 a tree fifteen metres tall would
// hold its finest level out past two hundred metres. These put the switches at about 11, 22 and
// 45 m. The wood's box is about 15% smaller than the sprays' it carries -- the sprays reach past
// the branches -- so it takes the larger scale to switch at the same distances.
#define WOODS_WOOD_LOD_SCALE  0.053f
#define WOODS_SPRAY_LOD_SCALE 0.049f

#define WOODS_X0    (CHASM_X - 2.0f) // the grid's west edge, at the chasm
#define SITE_STEP   5.0f             // the jittered grid's cell
#define SITE_JITTER 0.42f            // of a cell, either way
#define EDGE_THIN   5.0f             // metres in from the woods' edge over which they thin
#define EDGE_CLEAR  1.6f  // and the least a trunk stands from our back fences or the terrace's back
#define DRIVE_CLEAR 7.0f  // from the drive's centre line
#define CONIFER_MIN 0.10f // the generator's ~125 units to 12.5 m
#define CONIFER_MAX 0.15f
#define DEAD_MIN    0.05f
#define DEAD_MAX    0.075f
// An atlas cell: 1024 wide, as wide as the plant's, so the material array grows no wider for it.
#define NEEDLE_CELL 128

#define STUMPS      30
#define LOGS        40
#define BOULDERS    30
#define ROCK_MODELS 3

// The lake valley's woods (spec 13.41), on the same grid carried on west and south.
#define TRACK_CLEAR     5.0f    // metres from the track's centre line
#define SHORE_CLEAR     4.0f    // of bare bank at the water's edge
#define PAD_CLEAR       3.0f    // round the cabin's pad
#define LIP_CLEAR       2.5f    // from the chasm's lips, as the town's woods stand
#define VALLEY_EDGE     3.0f    // inside the ground's edge
#define ROCK_COS        0.8387f // and none where the ground is steeper than 33 degrees
#define VALLEY_STUMPS   10
#define VALLEY_LOGS     14
#define VALLEY_BOULDERS 30

// How far (x, z) is into the woods west of the street, round the crossroads (spec 13.35): clear of
// the lip, the cross street and the street, and of the barricades' lines.
static float west_depth(float x, float z) {
    const float lip = x - (land_lip_x(z) + 2.5f);
    const float cross = fabsf(x - CROSS_X) - 9.0f;
    const float street = fabsf(z) - (STREET_HALF_WIDTH + 3.0f);
    const float barricade = fminf(fabsf(z - CROSS_NORTH_Z), fabsf(z - CROSS_SOUTH_Z)) - 2.0f;
    return fminf(fminf(lip, cross), fminf(street, barricade));
}

// How far (x, z) is into the woods: positive inside, the distance to the nearest edge facing the
// houses, and negative outside them.
static float woods_depth(float x, float z) {
    if (x < -STREET_HALF_LEN)
        return west_depth(x, z);
    if (x > WOODS_EAST_X)
        return -1.0f;
    if (hill_drive_distance(x, z) < DRIVE_CLEAR)
        return -1.0f;
    // The terrace's east return runs up into the north woods along the street's east end.
    if (fabsf(x - STREET_HALF_LEN) < 1.4f && z < TERRACE_WALL_Z)
        return -1.0f;
    const float north = (TERRACE_BACK_Z - EDGE_CLEAR) - z;
    const float south = z - (BACK_FENCE_Z + EDGE_CLEAR);
    return fmaxf(north, south);
}

// How far (x, z) is into the lake valley's woods: clear of the track, the water's edge, the
// cabin's pad, the chasm's lips and the ground's edge, and negative outside them.
static float valley_depth(float x, float z) {
    const float corner = land_lip_x(RIDGE_Z);
    // The chasm: west of its east lip north of the corner, north of the ridge's lip west of it.
    const float lip = x < corner ? z - (land_ridge_lip_z(x) + LIP_CLEAR)
                                 : (z < RIDGE_Z ? x - (land_lip_x(z) + LIP_CLEAR) : FLT_MAX);
    const float edge = fminf(fminf(x - (VALLEY_X0 + VALLEY_EDGE), (VALLEY_X1 - VALLEY_EDGE) - x),
                             (VALLEY_Z1 - VALLEY_EDGE) - z);
    const float pad =
        plan_box_distance(x, z, CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1) - PAD_CLEAR;
    const float clear =
        fminf(lake_track_distance(x, z) - TRACK_CLEAR, lake_shore_distance(x, z) - SHORE_CLEAR);
    return fminf(fminf(lip, edge), fminf(pad, clear));
}

// Whether the ground at (x, z) is too steep to hold a tree: the ridge's flanks show rock.
static bool too_steep(float x, float z) {
    const float hx = 0.5f * (land_height(x + 1.0f, z) - land_height(x - 1.0f, z));
    const float hz = 0.5f * (land_height(x, z + 1.0f) - land_height(x, z - 1.0f));
    return 1.0f / sqrtf(1.0f + hx * hx + hz * hz) < ROCK_COS;
}

/*
 * A conifer's sprays: the needle atlas on alpha-tested cards drawn from both sides, casting their
 * cut-out shadow and moving in the wind -- held at rest in the cached shadows, as the dead trees
 * are.
 */
static Material* needles_material(Scene* scene, ShaderProgram* program) {
    Material* m = create_material();
    m->name = safe_strdup("woods_needles");
    m->roughness = 1.0f;
    m->foliage_shadows = 1;
    m->wind_mode = 2;
    // The wood's response, so a spray rides its branch; what made the sprays livelier than the
    // wood is their flutter about their stems.
    m->wind_response = TREES_WIND_RESPONSE;
    m->wind_flutter = 0.6f / TREES_WIND_RESPONSE;
    m->cached_shadow_wind = CACHED_SHADOW_WIND_REST;
    material_set_program(m, program);
    TextureDesc albedo = texture_desc(true);
    mats_cutout(m, 0.4f, &albedo);
    BakedMaps maps = {
        .width = NEEDLE_CELL * TG_LEAF_VARIANTS, .height = NEEDLE_CELL, .albedo_channels = 4};
    veg_needle_spray_maps(maps.width, maps.height, &maps.albedo, &maps.normal, &maps.rough);
    mats_set_baked(m, scene, "woods_needles", &maps, albedo);
    scene_add_material(scene, m);
    return m;
}

static SceneNode* group_node(SceneNode* parent) {
    SceneNode* g = create_node();
    node_add_child(parent, g);
    return g;
}

// A node drawing `mesh` at `m`, hung under `parent`.
static void place(SceneNode* parent, Mesh* mesh, const mat4 m) {
    SceneNode* node = create_node();
    glm_mat4_copy((vec4*)m, node->original_transform);
    node_add_mesh(node, mesh_ref(mesh));
    node_add_child(parent, node);
}

// A boulder: one of the rock models, squashed, turned and half sunk at (x, z), as faceted kit
// geometry in the cliff's rock, with a body round its bulk.
static void boulder(Kit* kit, const Mesh* rock, float x, float z, float size, float yaw) {
    const float squash = 0.65f;
    const float y = land_height(x, z) - 0.3f * size * squash;
    const KitFrame f = {{x, y, z}, yaw};
    vec3 centre = {x, y, z};
    for (size_t t = 0; t + 2 < rock->index_count; t += 3) {
        vec3 p[3] = {{0.0f}};
        for (int k = 0; k < 3; k++) {
            const float* v = &rock->vertices[3 * rock->indices[t + (size_t)k]];
            kit_frame_point(&f, v[0] * size, v[1] * size * squash, v[2] * size, p[k]);
        }
        vec3 mid = {0.0f, 0.0f, 0.0f}, out = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 3; k++)
            glm_vec3_add(mid, p[k], mid);
        glm_vec3_scale(mid, 1.0f / 3.0f, mid);
        glm_vec3_sub(mid, centre, out);
        kit_tri_facing(kit, MAT_CLIFF, p[0], p[1], p[2], out);
    }
    kit_collider(kit, (vec3){x, y + 0.35f * size * squash, z},
                 (vec3){0.7f * size, 0.6f * size * squash, 0.7f * size}, yaw);
}

// A stump: the trunk's foot, flared, cut off square, its cut face the old boards' end grain.
static void stump(Kit* kit, float x, float z, float r, float h) {
    const float y = land_height(x, z) - 0.05f;
    const vec2 profile[] = {{r * 1.35f, 0.0f}, {r * 1.08f, 0.12f * h}, {r, 0.4f * h}, {r, h}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_LOG, x, z, y, profile, KIT_COUNT(profile), 10);
    vec3 top[10];
    for (int i = 0; i < 10; i++) {
        const float a = 2.0f * GLM_PIf * (float)i / 10.0f;
        top[i][0] = x + r * cosf(a);
        top[i][1] = y + h;
        top[i][2] = z + r * sinf(a);
    }
    kit_polygon_facing(kit, MAT_PORCH, top, 10, (vec3){0.0f, 1.0f, 0.0f});
    kit_collider(kit, (vec3){x, y + 0.5f * h, z}, (vec3){r, 0.5f * h, r}, 0.0f);
}

// A log on the ground from (x, z) along `yaw`, `len` long, sagging into the ground's own lumps.
static void log_down(Kit* kit, float x, float z, float yaw, float len, float r) {
    const int n = 4;
    vec3 path[4];
    const float dx = sinf(yaw), dz = cosf(yaw);
    for (int i = 0; i < n; i++) {
        const float t = len * (float)i / (float)(n - 1);
        const float px = x + dx * t, pz = z + dz * t;
        path[i][0] = px;
        path[i][1] = land_height(px, pz) + 0.8f * r;
        path[i][2] = pz;
    }
    kit_frame_pipe(kit, &KIT_WORLD, MAT_LOG, path, n, r, 9);
    const float mid_y = 0.5f * (path[0][1] + path[n - 1][1]);
    kit_collider(kit, (vec3){x + 0.5f * len * dx, mid_y, z + 0.5f * len * dz},
                 (vec3){r, r + 0.2f, 0.5f * len}, yaw);
}

/*
 * A dead tree lying across a length of fence that has come down: it is what brought it down. Its
 * foot is out in the woods, its crown across the fallen boards into the yard; a body along its
 * trunk as far as the fence. The fence's own body still runs across the gap.
 */
static void deadfall(Kit* kit, Trees* trees, SceneNode* parent, const FenceBreach* br, int which,
                     unsigned int* state) {
    Mesh* mesh = trees_grow_still(trees, which);
    if (!mesh)
        return;
    const vec2 mid = {0.5f * (br->a[0] + br->b[0]), 0.5f * (br->a[1] + br->b[1])};
    vec2 along = {br->b[0] - br->a[0], br->b[1] - br->a[1]};
    glm_vec2_normalize(along);
    const float scale = 0.062f, length = trees->trunk_length * scale;
    const float base_out = length - 1.2f;
    const float skew =
        glm_rad(15.0f + 20.0f * kit_xrnd(state)) * (kit_xrnd(state) < 0.5f ? -1.0f : 1.0f);
    // Toward the yard, turned off square to the fence by the skew.
    vec2 dir = {-br->out[0] * cosf(skew) + along[0] * sinf(skew),
                -br->out[1] * cosf(skew) + along[1] * sinf(skew)};
    const float bx = mid[0] - dir[0] * base_out, bz = mid[1] - dir[1] * base_out;
    const float y = land_height(bx, bz) + 0.4f;
    const float yaw = atan2f(dir[0], dir[1]);
    const float sag = glm_rad(3.0f); // how far past level the trunk lies, crown down
    mat4 m;
    glm_translate_make(m, (vec3){bx, y, bz});
    // The crown crushed under it: what stood out all round the trunk now spreads along the
    // ground, its upper branches lower than they would stand.
    glm_scale(m, (vec3){1.0f, 0.5f, 1.0f});
    glm_rotate_y(m, yaw, m);
    // Laid down, its crown sloping to the ground and its foot held up by the root plate.
    glm_rotate_x(m, 0.5f * GLM_PIf + sag, m);
    glm_rotate_y(m, 2.0f * GLM_PIf * kit_xrnd(state), m);
    glm_scale_uni(m, scale);
    place(parent, mesh, m);
    free_mesh(mesh);
    // Its body as wide as its own trunk -- squashed by half in Y, the trunk keeps its radius
    // across -- and centred where the trunk is halfway to the fence, the squashed sag having
    // brought it down a little from its foot.
    const float half = 0.5f * base_out;
    const float r = trees->trunk_radius[which] * scale * TREES_TRUNK_BODY;
    kit_collider(kit, (vec3){bx + dir[0] * half, y - 0.5f * half * tanf(sag), bz + dir[1] * half},
                 (vec3){r, 0.6f, half}, yaw);
}

// What a site is planted from: the conifers' meshes and the groups their copies hang in, and the
// counts so far.
typedef struct Planting {
    Kit* kit;
    Trees* trees;
    Mesh** wood;
    Mesh** sprays;
    const float* trunk_radius;
    SceneNode** wood_groups;
    SceneNode** spray_groups;
    int conifers, dead;
} Planting;

// A tree at (x, z): the species `kind` falls in and the model within it, turned `yaw`, its size
// and lean from `size` and `lean`, each 0..1, and a body round its trunk.
static void plant(Planting* p, float x, float z, float kind, float yaw, float size, float lean) {
    const Species* sp = &SPECIES[0];
    float k = kind;
    while (k >= sp->share && sp < &SPECIES[KIT_COUNT(SPECIES) - 1]) {
        k -= sp->share;
        sp++;
    }
    const int model = sp->first + (int)(k / sp->share * (float)sp->models) % sp->models;
    Trees* trees = p->trees;
    mat4 m;
    if (sp->dead) {
        if (!trees->dead[model])
            return;
        trees_stand(trees, p->kit, model, x, z, DEAD_MIN + (DEAD_MAX - DEAD_MIN) * size, yaw,
                    glm_rad(2.0f + 6.0f * lean));
        p->dead++;
        return;
    }
    const int c = model;
    if (!p->wood[c])
        return;
    const float scale = CONIFER_MIN + (CONIFER_MAX - CONIFER_MIN) * size;
    const float tilt = glm_rad(2.0f * lean);
    glm_translate_make(m, (vec3){x, land_height(x, z) - 0.2f, z});
    glm_rotate_y(m, yaw, m);
    glm_rotate_x(m, tilt, m);
    glm_scale_uni(m, scale);
    place(p->wood_groups[c], p->wood[c], m);
    if (p->sprays[c])
        place(p->spray_groups[c], p->sprays[c], m);
    trees_trunk_collider(p->kit, x, z, land_height(x, z),
                         p->trunk_radius[c] * scale * TREES_TRUNK_BODY, TREES_TRUNK_HEIGHT, yaw,
                         tilt);
    p->conifers++;
}

/*
 * The lake valley's woods (spec 13.41): the town's grid of sites carried on west past the chasm's
 * turn and south past the woods behind our side, over the cells the town's pass does not plant --
 * and the old cutting south of the ridge's line, which that pass kept clear for the cross street.
 * From a stream of its own, so the town's woods are where they were. Then what lies on its
 * floor, the boulders down the bank to the water as well as in the trees.
 */
static void valley_woods(Planting* p, const Mesh* const* rocks, unsigned int seed, int* props) {
    unsigned int state = seed * 2246822519u + 0x1341u;
    const int town_cols = (int)ceilf((WOODS_EAST_X - WOODS_X0) / SITE_STEP);
    const int town_rows = (int)ceilf((WORLD_Z1 - WORLD_Z0) / SITE_STEP);
    const int i0 = (int)floorf((VALLEY_X0 - WOODS_X0) / SITE_STEP);
    const int i1 = (int)ceilf((VALLEY_X1 - WOODS_X0) / SITE_STEP);
    const int rows = (int)ceilf((VALLEY_Z1 - WORLD_Z0) / SITE_STEP);
    for (int j = 0; j < rows; j++)
        for (int i = i0; i < i1; i++) {
            const bool cutting =
                i >= 2 && i <= 4 && WORLD_Z0 + SITE_STEP * (float)j >= RIDGE_Z - 2.0f;
            if (i >= 0 && i < town_cols && j < town_rows && !cutting)
                continue;
            const float x = WOODS_X0 + SITE_STEP * ((float)i + 0.5f) +
                            SITE_STEP * SITE_JITTER * (2.0f * kit_xrnd(&state) - 1.0f);
            const float z = WORLD_Z0 + SITE_STEP * ((float)j + 0.5f) +
                            SITE_STEP * SITE_JITTER * (2.0f * kit_xrnd(&state) - 1.0f);
            const float kind = kit_xrnd(&state), yaw = 2.0f * GLM_PIf * kit_xrnd(&state);
            const float size = kit_xrnd(&state), lean = kit_xrnd(&state);
            const float keep = kit_xrnd(&state);
            const float depth = valley_depth(x, z);
            if (depth <= 0.0f || keep > 0.45f + 0.5f * glm_smoothstep(0.0f, EDGE_THIN, depth) ||
                too_steep(x, z))
                continue;
            plant(p, x, z, kind, yaw, size, lean);
        }

    int stumps = 0, logs = 0, boulders = 0;
    for (int tries = 0; tries < 4000 && (stumps < VALLEY_STUMPS || logs < VALLEY_LOGS ||
                                         boulders < VALLEY_BOULDERS);
         tries++) {
        const float x = VALLEY_X0 + (VALLEY_X1 - VALLEY_X0) * kit_xrnd(&state);
        const float z = RIDGE_Z + (VALLEY_Z1 - RIDGE_Z) * kit_xrnd(&state);
        const float a = kit_xrnd(&state), b = kit_xrnd(&state), c = kit_xrnd(&state);
        const float depth = valley_depth(x, z);
        // Boulders down the bank to the water's edge too, and on the ridge's rock.
        const float bank = lake_shore_distance(x, z);
        const bool rocky =
            (bank > 0.5f && bank < SHORE_CLEAR && lake_track_distance(x, z) > TRACK_CLEAR) ||
            (depth > 0.0f && too_steep(x, z));
        if (depth < 1.0f && !(rocky && boulders < VALLEY_BOULDERS))
            continue;
        if (!rocky && stumps < VALLEY_STUMPS) {
            stump(p->kit, x, z, 0.2f + 0.15f * a, 0.25f + 0.5f * b);
            stumps++;
        } else if (!rocky && logs < VALLEY_LOGS) {
            const float yaw = 2.0f * GLM_PIf * c, len = 2.5f + 4.5f * a;
            if (valley_depth(x + sinf(yaw) * len, z + cosf(yaw) * len) < 1.0f)
                continue;
            log_down(p->kit, x, z, yaw, len, 0.12f + 0.16f * b);
            logs++;
        } else if (boulders < VALLEY_BOULDERS) {
            const Mesh* rock = rocks[boulders % ROCK_MODELS];
            if (rock)
                boulder(p->kit, rock, x, z, 0.4f + 1.1f * a * a, 2.0f * GLM_PIf * b);
            boulders++;
        }
    }
    props[0] = stumps;
    props[1] = logs;
    props[2] = boulders;
}

void woods_build(Kit* kit, Engine* engine, Scene* scene, Trees* trees,
                 const FenceBreaches* breaches, unsigned int seed) {
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    Material* needles = needles_material(scene, pbr);
    // The loading screen moves between the needles' bake and each model grown.
    engine_draw_loading_screen(engine);
    Mesh *wood[CONIFER_MODELS], *sprays[CONIFER_MODELS];
    float trunk_radius[CONIFER_MODELS];
    for (int i = 0; i < CONIFER_MODELS; i++) {
        TreeParams p;
        tree_params_preset(&p, CONIFERS[i].preset, CONIFERS[i].seed);
        trunk_radius[i] = p.trunk_radius;
        trees_grow(&p, trees->bark, needles, &wood[i], &sprays[i]);
        if (wood[i])
            wood[i]->lod_scale = WOODS_WOOD_LOD_SCALE;
        if (sprays[i])
            sprays[i]->lod_scale = WOODS_SPRAY_LOD_SCALE;
        engine_draw_loading_screen(engine);
    }

    SceneNode* root = create_node();
    node_set_name(root, "woods");
    root->draw_distance = trees->reach;
    node_add_child(scene->root_node, root);
    SceneNode *wood_groups[CONIFER_MODELS], *spray_groups[CONIFER_MODELS];
    for (int i = 0; i < CONIFER_MODELS; i++)
        wood_groups[i] = group_node(root);
    for (int i = 0; i < CONIFER_MODELS; i++)
        spray_groups[i] = group_node(root);

    Planting planting = {.kit = kit,
                         .trees = trees,
                         .wood = wood,
                         .sprays = sprays,
                         .trunk_radius = trunk_radius,
                         .wood_groups = wood_groups,
                         .spray_groups = spray_groups};
    unsigned int state = seed * 2246822519u + 0x13355u;
    const int cols = (int)ceilf((WOODS_EAST_X - WOODS_X0) / SITE_STEP);
    const int rows = (int)ceilf((WORLD_Z1 - WORLD_Z0) / SITE_STEP);
    for (int j = 0; j < rows; j++)
        for (int i = 0; i < cols; i++) {
            const float x = WOODS_X0 + SITE_STEP * ((float)i + 0.5f) +
                            SITE_STEP * SITE_JITTER * (2.0f * kit_xrnd(&state) - 1.0f);
            const float z = WORLD_Z0 + SITE_STEP * ((float)j + 0.5f) +
                            SITE_STEP * SITE_JITTER * (2.0f * kit_xrnd(&state) - 1.0f);
            const float depth = woods_depth(x, z);
            const float keep = 0.45f + 0.5f * glm_smoothstep(0.0f, EDGE_THIN, depth);
            const float kind = kit_xrnd(&state), yaw = 2.0f * GLM_PIf * kit_xrnd(&state);
            const float size = kit_xrnd(&state), lean = kit_xrnd(&state);
            if (depth <= 0.0f || kit_xrnd(&state) > keep)
                continue;
            // Off the lake's track, once the site's draws are taken, so every other site is
            // where it was (spec 13.41).
            if (lake_track_distance(x, z) < TRACK_CLEAR)
                continue;
            plant(&planting, x, z, kind, yaw, size, lean);
        }
    const int town_conifers = planting.conifers, town_dead = planting.dead;

    // What lies on the floor, scattered over the same bands by darts that miss outside them.
    Mesh* rocks[ROCK_MODELS] = {NULL};
    for (int i = 0; i < ROCK_MODELS; i++) {
        RockParams rp = rock_default_params();
        rp.subdivisions = 2;
        rp.seed = 1335u + (unsigned)i;
        rocks[i] = create_mesh();
        if (!rock_build_mesh(&rp, rocks[i])) {
            free_mesh(rocks[i]);
            rocks[i] = NULL;
        }
    }
    int stumps = 0, logs = 0, boulders = 0;
    for (int tries = 0; tries < 4000 && (stumps < STUMPS || logs < LOGS || boulders < BOULDERS);
         tries++) {
        const float x = WOODS_X0 + (WOODS_EAST_X - WOODS_X0) * kit_xrnd(&state);
        const float z = WORLD_Z0 + (WORLD_Z1 - WORLD_Z0) * kit_xrnd(&state);
        const float a = kit_xrnd(&state), b = kit_xrnd(&state), c = kit_xrnd(&state);
        if (woods_depth(x, z) < 1.0f)
            continue;
        // Counted but left off the lake's track, so the darts after it land where they did.
        const bool on_track = lake_track_distance(x, z) < TRACK_CLEAR;
        if (stumps < STUMPS) {
            if (!on_track)
                stump(kit, x, z, 0.2f + 0.15f * a, 0.25f + 0.5f * b);
            stumps++;
        } else if (logs < LOGS) {
            const float yaw = 2.0f * GLM_PIf * c, len = 2.5f + 4.5f * a;
            const float ex = x + sinf(yaw) * len, ez = z + cosf(yaw) * len;
            if (woods_depth(ex, ez) < 1.0f)
                continue;
            if (!on_track && lake_track_distance(ex, ez) >= TRACK_CLEAR)
                log_down(kit, x, z, yaw, len, 0.12f + 0.16f * b);
            logs++;
        } else {
            const Mesh* rock = rocks[boulders % ROCK_MODELS];
            if (rock && !on_track)
                boulder(kit, rock, x, z, 0.4f + 1.1f * a * a, 2.0f * GLM_PIf * b);
            boulders++;
        }
    }

    for (int i = 0; i < breaches->count; i++)
        deadfall(kit, trees, root, &breaches->at[i], i * 2 + 1, &state);

    int valley_props[3] = {0};
    valley_woods(&planting, (const Mesh* const*)rocks, seed, valley_props);
    for (int i = 0; i < ROCK_MODELS; i++)
        if (rocks[i])
            free_mesh(rocks[i]);
    for (int i = 0; i < CONIFER_MODELS; i++) {
        if (wood[i])
            free_mesh(wood[i]);
        if (sprays[i])
            free_mesh(sprays[i]);
    }

    printf("silent: woods of %d conifers and %d dead trees from %d models, %d stumps, %d logs, "
           "%d boulders, %d deadfalls\n",
           town_conifers, town_dead, CONIFER_MODELS, stumps, logs, boulders, breaches->count);
    printf("silent: the lake valley's woods of %d conifers and %d dead trees, %d stumps, %d logs, "
           "%d boulders\n",
           planting.conifers - town_conifers, planting.dead - town_dead, valley_props[0],
           valley_props[1], valley_props[2]);
}
