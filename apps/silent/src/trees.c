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
#include "layout.h"
#include "trees.h"

/*
 * Dead trees (spec 13.25). The engine's generator grows them with no leaves, and its shape knobs
 * are pushed toward what a dead tree is: branches that sag (droop), wander (curve noise) and do
 * not straighten back toward the light (low phototropism), spread wide and sparse. A handful of
 * models, each grown once at the generator's native size and stood about the hill many times
 * over at a scale, turned and leaning a little.
 *
 * Their own nodes rather than kit geometry: a tree sways in the wind, and a material with wind is
 * one the kit's shadow cells leave out.
 */

#define TREE_MODELS 6
// The generator's native trunk is ~125 units; a dead tree here is 7 to 11 m tall.
#define TREE_SCALE_MIN 0.045f
#define TREE_SCALE_MAX 0.07f
#define TREE_TRUNK     8.0f // native trunk radius
#define BARK_SIZE      256  // the procedural bark's edge, at silent's texel density

// Where trees stand: down both sides of the drive, out of the way of it, and round the grounds.
#define TREE_DRIVE_NEAR 4.5f // metres from the drive's centre line, at the least
#define TREE_DRIVE_FAR  11.0f
#define TREE_ATTEMPTS   900
#define TREE_COUNT      110

static Material* bark_material(Scene* scene, ShaderProgram* program) {
    Material* m = create_material();
    m->name = strdup("dead_bark");
    // Grey-brown and weathered: wood that has been dead and wet a long time.
    glm_vec3_copy((vec3){0.42f, 0.38f, 0.34f}, m->albedo);
    m->roughness = 0.85f;
    m->wind_mode = 1;         // the trunk leans and the branches sway; there are no leaves
    m->wind_response = 0.35f; // stiff: dead wood does not give much
    // A candle's or a lamp's cached shadow holds a tree at rest, off by at most its sway, rather
    // than drawing every face a tree reaches again on every frame.
    m->cached_shadow_wind = CACHED_SHADOW_WIND_REST;
    material_set_program(m, program);

    float* field = malloc(sizeof(float) * BARK_SIZE * BARK_SIZE);
    if (field) {
        veg_bark_height_field(field, BARK_SIZE, BARK_SIZE);
        unsigned char* albedo = veg_bark_albedo(BARK_SIZE, BARK_SIZE, field);
        unsigned char* normal = veg_bark_normal(BARK_SIZE, BARK_SIZE, field);
        unsigned char* rough = veg_bark_roughness(BARK_SIZE, BARK_SIZE, field);
        free(field);
        if (albedo)
            material_set_albedo_tex(
                m, texture_load_memory_owned(scene->tex_pool, "silent_bark_albedo", albedo,
                                             BARK_SIZE, BARK_SIZE, 3, texture_desc(true)));
        if (normal)
            material_set_normal_tex(
                m, texture_load_memory_owned(scene->tex_pool, "silent_bark_normal", normal,
                                             BARK_SIZE, BARK_SIZE, 3,
                                             (TextureDesc){.is_srgb = false,
                                                           .alpha = TEXTURE_ALPHA_DATA,
                                                           .use = TEXTURE_USE_NORMAL}));
        if (rough)
            material_set_roughness_tex(
                m, texture_load_memory_owned(scene->tex_pool, "silent_bark_rough", rough, BARK_SIZE,
                                             BARK_SIZE, 3, texture_desc(false)));
    }
    scene_add_material(scene, m);
    return m;
}

static Mesh* grow(int model, Material* bark) {
    TreeParams tp;
    memset(&tp, 0, sizeof(tp));
    tp.seed = 4049 + model * 61;
    tp.max_depth = 3;
    tp.trunk_length = 125.0f;
    tp.trunk_radius = TREE_TRUNK + (float)(model % 3);
    tp.branches_per_node = 2 + model % 2;
    tp.length_decay = 0.7f;
    tp.taper = 0.5f;
    tp.branch_angle = 42.0f + (float)(model % 3) * 8.0f;
    tp.angle_variance = 22.0f;
    tp.twist = 137.5f;
    tp.droop = 0.55f;
    tp.curve_noise = 0.7f;
    tp.phototropism = 0.1f;
    tp.lateral_density = 0.45f;
    tp.twig_scale = 0.8f;
    tp.show_leaves = 0;

    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, &tp);
    Mesh* mesh = create_mesh();
    if (!tree_mesh_bark(&skel, &tp, mesh)) {
        free_mesh(mesh);
        mesh = NULL;
    } else {
        mesh->material = bark;
    }
    tree_skeleton_free(&skel);
    return mesh;
}

// A small hash of an RNG, so the trees draw nothing from the street's and every seed builds the
// same street.
static float rnd(unsigned int* state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return (float)(*state & 0xffffffu) / 16777215.0f;
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

void trees_build(Kit* kit, Engine* engine, Scene* scene, unsigned int seed) {
    Material* bark = bark_material(scene, engine_get_program(engine, CETRA_PROGRAM_PBR));
    Mesh* models[TREE_MODELS];
    for (int i = 0; i < TREE_MODELS; i++)
        models[i] = grow(i, bark);

    SceneNode* group = create_node();
    node_set_name(group, "dead_trees");
    node_add_child(scene->root_node, group);

    unsigned int state = seed * 2654435761u + 0x7f4a7c15u;
    int placed = 0;
    for (int attempt = 0; attempt < TREE_ATTEMPTS && placed < TREE_COUNT; attempt++) {
        // Mostly hugging the drive, the rest scattered over the hill.
        float x, z;
        if (rnd(&state) < 0.75f) {
            hill_drive_point(rnd(&state), &x, &z);
            const float side = rnd(&state) < 0.5f ? -1.0f : 1.0f;
            const float off = TREE_DRIVE_NEAR + (TREE_DRIVE_FAR - TREE_DRIVE_NEAR) * rnd(&state);
            const float angle = rnd(&state) * 6.2831853f;
            x += side * off * cosf(angle);
            z += side * off * sinf(angle);
        } else {
            x = STREET_HALF_LEN + (WORLD_X1 - STREET_HALF_LEN) * rnd(&state);
            z = WORLD_Z0 + (WORLD_Z1 - WORLD_Z0) * rnd(&state);
        }
        if (!free_ground(x, z))
            continue;
        Mesh* model = models[placed % TREE_MODELS];
        if (!model)
            continue;

        const float scale = TREE_SCALE_MIN + (TREE_SCALE_MAX - TREE_SCALE_MIN) * rnd(&state);
        const float yaw = rnd(&state) * 6.2831853f;
        // A lean of a few degrees, as a dead tree has, and sunk a little into the ground so the
        // flare of its roots is in it.
        const float lean = glm_rad(2.0f + 6.0f * rnd(&state));
        const float y = hill_height(x, z) - 0.25f;
        SceneNode* node = create_node();
        mat4 m;
        glm_translate_make(m, (vec3){x, y, z});
        glm_rotate_y(m, yaw, m);
        glm_rotate_x(m, lean, m);
        glm_scale_uni(m, scale);
        glm_mat4_copy(m, node->original_transform);
        node_add_mesh(node, mesh_ref(model));
        node_add_child(group, node);

        const float r = TREE_TRUNK * scale * 0.8f;
        kit_collider(kit, (vec3){x, y + 1.5f, z}, (vec3){r, 1.5f, r}, 0.0f);
        placed++;
    }
    for (int i = 0; i < TREE_MODELS; i++)
        if (models[i])
            free_mesh(models[i]);
    printf("silent: %d dead trees from %d models\n", placed, TREE_MODELS);
}
