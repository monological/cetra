#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/procedural/rock.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "hill.h"
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
 * detail -- the wood by the engine's simplifier, the sprays by thinning them -- with silent's LOD
 * bias (silent.c) set for these, the only chains in the app.
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

#define WOODS_X0    (CHASM_X - 2.0f) // the grid's west edge, at the chasm
#define SITE_STEP   5.0f             // the jittered grid's cell
#define SITE_JITTER 0.42f            // of a cell, either way
#define EDGE_THIN   5.0f             // metres in from the woods' edge over which they thin
#define EDGE_CLEAR  1.6f             // and the least a trunk stands from a fence
#define DRIVE_CLEAR 7.0f             // from the drive's centre line
#define CONIFER_MIN 0.10f            // the generator's ~125 units to 12.5 m
#define CONIFER_MAX 0.15f
#define DEAD_MIN    0.05f
#define DEAD_MAX    0.075f
#define DEAD_SHARE  0.10f
#define SNAG_SHARE  0.06f
// An atlas cell: 1024 wide, as wide as the plant's, so the material array grows no wider for it.
#define NEEDLE_CELL 128

#define STUMPS      30
#define LOGS        40
#define BOULDERS    30
#define ROCK_MODELS 3

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

/*
 * A conifer's sprays: the needle atlas on alpha-tested cards drawn from both sides, casting their
 * cut-out shadow and moving in the wind -- held at rest in the cached shadows, as the dead trees
 * are.
 */
static Material* needles_material(Scene* scene, ShaderProgram* program) {
    const int w = NEEDLE_CELL * TG_LEAF_VARIANTS, h = NEEDLE_CELL;
    const float cutoff = 0.4f;
    Material* m = create_material();
    m->name = safe_strdup("woods_needles");
    m->roughness = 1.0f;
    m->alpha_mode = ALPHA_MASK;
    m->alphaCutoff = cutoff;
    m->doubleSided = true;
    m->foliage_shadows = 1;
    m->wind_mode = 2;
    m->wind_response = 0.6f;
    m->cached_shadow_wind = CACHED_SHADOW_WIND_REST;
    material_set_program(m, program);
    unsigned char *albedo = NULL, *normal = NULL, *rough = NULL;
    veg_needle_spray_maps(w, h, &albedo, &normal, &rough);
    TextureDesc albedo_desc = texture_desc(true);
    albedo_desc.coverage_cutoff = cutoff;
    if (albedo)
        material_set_albedo_tex(m,
                                texture_load_memory_owned(scene->tex_pool, "woods_needles_albedo",
                                                          albedo, w, h, 4, albedo_desc));
    if (normal)
        material_set_normal_tex(
            m, texture_load_memory_owned(scene->tex_pool, "woods_needles_normal", normal, w, h, 3,
                                         (TextureDesc){.is_srgb = false,
                                                       .alpha = TEXTURE_ALPHA_DATA,
                                                       .use = TEXTURE_USE_NORMAL}));
    if (rough)
        material_set_roughness_tex(m,
                                   texture_load_memory_owned(scene->tex_pool, "woods_needles_rough",
                                                             rough, w, h, 3, texture_desc(false)));
    scene_add_material(scene, m);
    return m;
}

// One conifer model's wood and sprays, each with its levels of detail.
static void grow_conifer(int model, Material* bark, Material* needles, Mesh** wood, Mesh** sprays) {
    TreeParams p;
    tree_params_preset(&p, CONIFERS[model].preset, CONIFERS[model].seed);
    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, &p);
    *wood = create_mesh();
    if (tree_mesh_bark(&skel, &p, *wood)) {
        (*wood)->material = bark;
    } else {
        free_mesh(*wood);
        *wood = NULL;
    }
    *sprays = create_mesh();
    if (tree_mesh_leaves(&skel, &p, *sprays)) {
        (*sprays)->material = needles;
    } else {
        free_mesh(*sprays);
        *sprays = NULL;
    }
    tree_skeleton_free(&skel);
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
    const float c = cosf(yaw), s = sinf(yaw);
    vec3 centre = {x, y, z};
    for (size_t t = 0; t + 2 < rock->index_count; t += 3) {
        vec3 p[3] = {{0.0f}};
        for (int k = 0; k < 3; k++) {
            const float* v = &rock->vertices[3 * rock->indices[t + (size_t)k]];
            const float lx = v[0] * size, ly = v[1] * size * squash, lz = v[2] * size;
            p[k][0] = x + lx * c + lz * s;
            p[k][1] = y + ly;
            p[k][2] = z - lx * s + lz * c;
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
    const float scale = 0.062f, length = 125.0f * scale;
    const float base_out = length - 1.2f;
    const float skew =
        glm_rad(15.0f + 20.0f * kit_xrnd(state)) * (kit_xrnd(state) < 0.5f ? -1.0f : 1.0f);
    // Toward the yard, turned off square to the fence by the skew.
    vec2 dir = {-br->out[0] * cosf(skew) + along[0] * sinf(skew),
                -br->out[1] * cosf(skew) + along[1] * sinf(skew)};
    const float bx = mid[0] - dir[0] * base_out, bz = mid[1] - dir[1] * base_out;
    const float y = land_height(bx, bz) + 0.4f;
    const float yaw = atan2f(dir[0], dir[1]);
    mat4 m;
    glm_translate_make(m, (vec3){bx, y, bz});
    // The crown crushed under it: what stood out all round the trunk now spreads along the
    // ground, its upper branches lower than they would stand.
    glm_scale(m, (vec3){1.0f, 0.5f, 1.0f});
    glm_rotate_y(m, yaw, m);
    // Laid down, its crown sloping to the ground and its foot held up by the root plate.
    glm_rotate_x(m, 0.5f * GLM_PIf + glm_rad(3.0f), m);
    glm_rotate_y(m, 2.0f * GLM_PIf * kit_xrnd(state), m);
    glm_scale_uni(m, scale);
    place(parent, mesh, m);
    free_mesh(mesh);
    kit_collider(kit, (vec3){bx + dir[0] * 0.5f * base_out, y, bz + dir[1] * 0.5f * base_out},
                 (vec3){0.45f, 0.6f, 0.5f * base_out}, yaw);
}

void woods_build(Kit* kit, Engine* engine, Scene* scene, Trees* trees,
                 const FenceBreaches* breaches, unsigned int seed) {
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    Material* needles = needles_material(scene, pbr);
    Mesh *wood[CONIFER_MODELS], *sprays[CONIFER_MODELS];
    for (int i = 0; i < CONIFER_MODELS; i++)
        grow_conifer(i, trees->bark, needles, &wood[i], &sprays[i]);

    SceneNode* root = create_node();
    node_set_name(root, "woods");
    node_add_child(scene->root_node, root);
    SceneNode *wood_groups[CONIFER_MODELS], *spray_groups[CONIFER_MODELS],
        *dead_groups[TREE_MODELS];
    for (int i = 0; i < CONIFER_MODELS; i++)
        wood_groups[i] = group_node(root);
    for (int i = 0; i < CONIFER_MODELS; i++)
        spray_groups[i] = group_node(root);
    for (int i = 0; i < TREE_MODELS; i++)
        dead_groups[i] = group_node(root);

    unsigned int state = seed * 2246822519u + 0x13355u;
    const int cols = (int)ceilf((WOODS_EAST_X - WOODS_X0) / SITE_STEP);
    const int rows = (int)ceilf((WORLD_Z1 - WORLD_Z0) / SITE_STEP);
    int conifers = 0, dead = 0;
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
            mat4 m;
            if (kind < DEAD_SHARE) {
                const int d = (int)(kind / DEAD_SHARE * (float)TREE_MODELS) % TREE_MODELS;
                if (!trees->dead[d])
                    continue;
                const float scale = DEAD_MIN + (DEAD_MAX - DEAD_MIN) * size;
                glm_translate_make(m, (vec3){x, land_height(x, z) - 0.25f, z});
                glm_rotate_y(m, yaw, m);
                glm_rotate_x(m, glm_rad(2.0f + 6.0f * lean), m);
                glm_scale_uni(m, scale);
                place(dead_groups[d], trees->dead[d], m);
                const float r = TREE_TRUNK * scale * 0.8f;
                kit_collider(kit, (vec3){x, land_height(x, z) + 1.5f, z}, (vec3){r, 1.5f, r}, 0.0f);
                dead++;
            } else {
                const int c = kind < DEAD_SHARE + SNAG_SHARE
                                  ? SNAG_MODEL
                                  : (int)((kind - DEAD_SHARE - SNAG_SHARE) /
                                          (1.0f - DEAD_SHARE - SNAG_SHARE) * (float)SNAG_MODEL) %
                                        SNAG_MODEL;
                if (!wood[c])
                    continue;
                const float scale = CONIFER_MIN + (CONIFER_MAX - CONIFER_MIN) * size;
                glm_translate_make(m, (vec3){x, land_height(x, z) - 0.2f, z});
                glm_rotate_y(m, yaw, m);
                glm_rotate_x(m, glm_rad(2.0f * lean), m);
                glm_scale_uni(m, scale);
                place(wood_groups[c], wood[c], m);
                if (sprays[c])
                    place(spray_groups[c], sprays[c], m);
                kit_collider(kit, (vec3){x, land_height(x, z) + 1.5f, z},
                             (vec3){0.35f, 1.5f, 0.35f}, 0.0f);
                conifers++;
            }
        }
    for (int i = 0; i < CONIFER_MODELS; i++) {
        if (wood[i])
            free_mesh(wood[i]);
        if (sprays[i])
            free_mesh(sprays[i]);
    }

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
        if (stumps < STUMPS) {
            stump(kit, x, z, 0.2f + 0.15f * a, 0.25f + 0.5f * b);
            stumps++;
        } else if (logs < LOGS) {
            const float yaw = 2.0f * GLM_PIf * c, len = 2.5f + 4.5f * a;
            if (woods_depth(x + sinf(yaw) * len, z + cosf(yaw) * len) < 1.0f)
                continue;
            log_down(kit, x, z, yaw, len, 0.12f + 0.16f * b);
            logs++;
        } else if (rocks[boulders % ROCK_MODELS]) {
            boulder(kit, rocks[boulders % ROCK_MODELS], x, z, 0.4f + 1.1f * a * a,
                    2.0f * GLM_PIf * b);
            boulders++;
        } else {
            boulders++;
        }
    }
    for (int i = 0; i < ROCK_MODELS; i++)
        if (rocks[i])
            free_mesh(rocks[i]);

    for (int i = 0; i < breaches->count; i++)
        deadfall(kit, trees, root, &breaches->at[i], i * 2 + 1, &state);

    printf("silent: woods of %d conifers and %d dead trees from %d models, %d stumps, %d logs, "
           "%d boulders, %d deadfalls\n",
           conifers, dead, CONIFER_MODELS, stumps, logs, boulders, breaches->count);
}
