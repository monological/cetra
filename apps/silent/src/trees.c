#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/engine.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/program.h"
#include "cetra/texture.h"

#include "hill.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "trees.h"

/*
 * Dead trees (spec 13.25): a handful of models of the engine's dead tree, each grown once at the
 * generator's native size and stood about the hill many times over at a scale, turned and leaning
 * a little.
 *
 * Their own nodes rather than kit geometry: a tree sways in the wind, and a material with wind is
 * one the kit's shadow cells leave out.
 */

// The generator's native trunk is ~125 units; a dead tree here is 7 to 11 m tall.
#define TREE_SCALE_MIN 0.045f
#define TREE_SCALE_MAX 0.07f
#define BARK_SIZE      256 // the procedural bark's edge, at silent's texel density

// Where trees stand: down both sides of the drive, out of the way of it, and round the grounds.
#define TREE_DRIVE_NEAR 4.5f // metres from the drive's centre line, at the least
#define TREE_DRIVE_FAR  11.0f
#define TREE_ATTEMPTS   900
#define TREE_COUNT      110

/*
 * The dead trees' bark, grey-brown and weathered: wood that has been dead and wet a long time.
 * With no `still_of` it bakes its maps and sways in the wind. Otherwise it is the same bark on wood
 * lying still -- `still_of`'s maps and no wind, which would bend a fallen trunk about its own
 * length as though it still stood.
 */
static Material* bark_material(Scene* scene, ShaderProgram* program, const Material* still_of) {
    Material* m = create_material();
    m->name = strdup(still_of ? "dead_bark_still" : "dead_bark");
    glm_vec3_copy((vec3){0.42f, 0.38f, 0.34f}, m->albedo);
    m->roughness = 0.85f;
    material_set_program(m, program);
    if (still_of) {
        material_set_albedo_tex(m, still_of->albedo_tex);
        material_set_normal_tex(m, still_of->normal_tex);
        material_set_roughness_tex(m, still_of->roughness_tex);
        scene_add_material(scene, m);
        return m;
    }
    m->wind_mode = 1; // the trunk leans and the branches sway
    m->wind_response = TREES_WIND_RESPONSE;
    // A candle's or a lamp's cached shadow holds a tree at rest, off by at most its sway, rather
    // than drawing every face a tree reaches again on every frame.
    m->cached_shadow_wind = CACHED_SHADOW_WIND_REST;

    float* field = malloc(sizeof(float) * BARK_SIZE * BARK_SIZE);
    if (field) {
        veg_bark_height_field(field, BARK_SIZE, BARK_SIZE);
        const BakedMaps maps = {veg_bark_albedo(BARK_SIZE, BARK_SIZE, field),
                                veg_bark_normal(BARK_SIZE, BARK_SIZE, field),
                                veg_bark_roughness(BARK_SIZE, BARK_SIZE, field),
                                BARK_SIZE,
                                BARK_SIZE,
                                3};
        free(field);
        mats_set_baked(m, scene, "silent_bark", &maps, texture_desc(true));
    }
    scene_add_material(scene, m);
    return m;
}

// A mesh `make` built from the skeleton, in `mat`; NULL when it built nothing.
static Mesh* grown(bool (*make)(const TreeSkeleton*, const TreeParams*, Mesh*),
                   const TreeSkeleton* skel, const TreeParams* p, Material* mat) {
    Mesh* mesh = create_mesh();
    if (!make(skel, p, mesh)) {
        free_mesh(mesh);
        return NULL;
    }
    mesh->material = mat;
    return mesh;
}

void trees_grow(const TreeParams* p, Material* bark, Material* foliage, Mesh** wood,
                Mesh** leaves) {
    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, p);
    *wood = grown(tree_mesh_bark, &skel, p, bark);
    if (leaves)
        *leaves = grown(tree_mesh_leaves, &skel, p, foliage);
    tree_skeleton_free(&skel);
}

// Dead model `model`: the engine's dead tree, each model a little stouter, bushier or wider.
static void dead_params(TreeParams* tp, int model) {
    tree_params_preset(tp, TREE_PRESET_DEAD, 4049 + model * 61);
    tp->trunk_radius += (float)(model % 3);
    tp->recursive.branches_per_node += model % 2;
    tp->recursive.branch_angle += (float)(model % 3) * 8.0f;
}

static Mesh* grow(int model, Material* bark) {
    TreeParams tp;
    dead_params(&tp, model);
    Mesh* mesh = NULL;
    trees_grow(&tp, bark, NULL, &mesh, NULL);
    return mesh;
}

// Whether (x, z) is somewhere a tree may stand: off the drive and its shoulders, out of the
// mansion's grounds inside the fence, out of the clearing before its gate -- the last stretch of
// the drive is where the house is first seen whole -- and inside the world.
#define GATE_CLEARING 14.0f
static bool free_ground(float x, float z) {
    if (x < STREET_HALF_LEN + 4.0f || x > WORLD_X1 - 3.0f || z < WORLD_Z0 + 3.0f ||
        z > WORLD_Z1 - 3.0f)
        return false;
    if (x > MANSION_X - 16.0f && x < MANSION_X + 16.0f && z > MANSION_Z - 3.0f &&
        z < MANSION_Z + 27.0f)
        return false;
    if (hypotf(x - MANSION_X, z - GROUNDS_Z0) < GATE_CLEARING)
        return false;
    return hill_drive_distance(x, z) > TREE_DRIVE_NEAR;
}

void trees_init(Trees* trees, Engine* engine, Scene* scene) {
    *trees = (Trees){0};
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    trees->bark = bark_material(scene, pbr, NULL);
    trees->still_bark = bark_material(scene, pbr, trees->bark);
    for (int i = 0; i < TREE_MODELS; i++) {
        trees->dead[i] = grow(i, trees->bark);
        TreeParams tp;
        dead_params(&tp, i);
        trees->trunk_length = tp.trunk_length;
        trees->trunk_radius[i] = tp.trunk_radius;
    }
}

void trees_trunk_collider(Kit* kit, float x, float z, float ground, float radius, float height,
                          float yaw, float lean) {
    // Tipped about X after the yaw, the trunk's axis runs (sin lean sin yaw, cos lean,
    // sin lean cos yaw), so at half the height it stands this far off its foot.
    const float off = 0.5f * height * tanf(lean);
    kit_collider(kit, (vec3){x + off * sinf(yaw), ground + 0.5f * height, z + off * cosf(yaw)},
                 (vec3){radius, 0.5f * height, radius}, 0.0f);
}

Mesh* trees_grow_still(Trees* trees, int model) {
    return grow(model % TREE_MODELS, trees->still_bark);
}

void trees_release(Trees* trees) {
    for (int i = 0; i < TREE_MODELS; i++) {
        if (trees->dead[i])
            free_mesh(trees->dead[i]);
        trees->dead[i] = NULL;
    }
}

void trees_build(Trees* trees, Kit* kit, Scene* scene, unsigned int seed) {
    Mesh* const* models = trees->dead;
    SceneNode* root = create_node();
    node_set_name(root, "dead_trees");
    root->draw_distance = trees->reach;
    node_add_child(scene->root_node, root);
    SceneNode** groups = trees->groups;
    for (int i = 0; i < TREE_MODELS; i++) {
        groups[i] = create_node();
        node_add_child(root, groups[i]);
    }

    unsigned int state = seed * 2654435761u + 0x7f4a7c15u;
    int placed = 0;
    for (int attempt = 0; attempt < TREE_ATTEMPTS && placed < TREE_COUNT; attempt++) {
        // Mostly hugging the drive, the rest scattered over the hill.
        float x, z;
        if (kit_xrnd(&state) < 0.75f) {
            hill_drive_point(kit_xrnd(&state), &x, &z);
            const float side = kit_xrnd(&state) < 0.5f ? -1.0f : 1.0f;
            const float off =
                TREE_DRIVE_NEAR + (TREE_DRIVE_FAR - TREE_DRIVE_NEAR) * kit_xrnd(&state);
            const float angle = kit_xrnd(&state) * 6.2831853f;
            x += side * off * cosf(angle);
            z += side * off * sinf(angle);
        } else {
            x = STREET_HALF_LEN + (WORLD_X1 - STREET_HALF_LEN) * kit_xrnd(&state);
            z = WORLD_Z0 + (WORLD_Z1 - WORLD_Z0) * kit_xrnd(&state);
        }
        if (!free_ground(x, z))
            continue;
        const int which = placed % TREE_MODELS;
        if (!models[which])
            continue;

        const float scale = TREE_SCALE_MIN + (TREE_SCALE_MAX - TREE_SCALE_MIN) * kit_xrnd(&state);
        const float yaw = kit_xrnd(&state) * 6.2831853f;
        // A lean of a few degrees, as a dead tree has.
        const float lean = glm_rad(2.0f + 6.0f * kit_xrnd(&state));
        trees_stand(trees, kit, which, x, z, scale, yaw, lean);
        placed++;
    }
    printf("silent: %d dead trees from %d models\n", placed, TREE_MODELS);
}

void trees_stand(Trees* trees, Kit* kit, int which, float x, float z, float scale, float yaw,
                 float lean) {
    const float y = land_height(x, z) - 0.25f;
    SceneNode* node = create_node();
    mat4 m;
    glm_translate_make(m, (vec3){x, y, z});
    glm_rotate_y(m, yaw, m);
    glm_rotate_x(m, lean, m);
    glm_scale_uni(m, scale);
    glm_mat4_copy(m, node->original_transform);
    node_add_mesh(node, mesh_ref(trees->dead[which]));
    node_add_child(trees->groups[which], node);
    trees_trunk_collider(kit, x, z, land_height(x, z),
                         trees->trunk_radius[which] * scale * TREES_TRUNK_BODY, TREES_TRUNK_HEIGHT,
                         yaw, lean);
}
