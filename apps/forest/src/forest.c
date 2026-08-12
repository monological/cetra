/*
 * forest -- a walkable kilometre of terrain, trees and rocks.
 *
 * Built to exercise spec 11.28's submission path with content rather than with
 * fixtures. Everything here is arranged so that instancing, LOD and frustum
 * culling all matter at once:
 *
 *   - a few PROTOTYPE meshes shared by thousands of nodes, so runs batch
 *   - prototypes grouped into contiguous sibling blocks, because the batcher
 *     joins only ADJACENT items and one foreign mesh between two instances ends
 *     the run
 *   - terrain split into tiles, because a tile is the unit of both culling and
 *     LOD selection and a kilometre-wide mesh is neither
 *   - no wind on anything scattered: a wind material is DRAW_UNBOUNDED, which
 *     exempts it from frustum culling entirely
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// glew before GLFW, or GLFW pulls the system gl.h in first and glew refuses.
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/camera.h"
#include "cetra/engine.h"
#include "cetra/ibl.h"
#include "cetra/light.h"
#include "cetra/lod.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/noise.h"
#include "cetra/profiler.h"
#include "cetra/program.h"
#include "cetra/render.h"
#include "cetra/scene.h"
#include "cetra/shadow.h"
#include "cetra/sky.h"
#include "cetra/transform.h"
#include "cetra/util.h"

#include "cetra/game/character.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

#include "cetra/procedural/rock.h"
#include "cetra/procedural/terrain.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/texture.h"

// --- scene scale -----------------------------------------------------------

#define TREE_PROTOTYPES 6
#define ROCK_PROTOTYPES 8
#define TREE_COUNT      2000
#define ROCK_COUNT      3000

// Collider resolution. Higher than it needs to be for Jolt and lower than the
// visual tiles, which is the trade: the character stands on this while the eye
// sees the tiles, and they diverge by about the height change across one
// collider quad.
#define COLLIDER_SEGMENTS 256

// Trees are generated at the generator's native scale (trunk_length ~125) and
// scaled down per instance rather than generated small. Leaf density is quoted
// per 10 units of arc, so a generator asked for a 12-unit trunk produces a bare
// canopy; scaling the node keeps the shape the parameters describe.
#define TREE_WORLD_SCALE 0.085f

typedef struct ForestArgs {
    int headless;
    int frames;
    const char* screenshot;
    int profiler;
    int no_lod;
    int no_instancing;
    int render_mode;     // RenderMode override; 0 = PBR
    int no_spatial_sort; // scatter in draw order rather than Morton order
    int width, height;   // 0 = the default window size
    int no_sky;          // swap the atmosphere for a plain directional rig
    float sun_elevation; // degrees; < -900 keeps the app default
    float sun_azimuth;
    int no_aerial; // keep the sky, drop aerial perspective
    int no_fog;       // volumetric fog is on by default
    int trace_player; // log position, ground state and velocity each second
    float lod_bias;
    unsigned seed;
    int cam_set;
    vec3 cam_eye;
    vec3 cam_target;
} ForestArgs;

static ForestArgs g_args;

static TerrainParams g_terrain;
static Scene* g_scene;
static SceneNode* g_root;
static Entity* g_player;
static ShaderProgram* g_pbr;

static Material* g_mat_terrain;
static Material* g_mat_bark;
static Material* g_mat_leaf;
static Material* g_mat_rock;

// Third-person orbit around the character. There is no follow-camera helper in
// cetra -- app.h offers only the mouse-drag orbit controller, which orbits a
// fixed point rather than a moving one.
static float g_cam_yaw = 0.6f;
static float g_cam_pitch = 0.28f;
static float g_cam_dist = 14.0f;

// Reported at startup, and the numbers the gate arms read from the log.
static size_t g_distinct_meshes;
static size_t g_prototype_tris;
static size_t g_node_count;
static size_t g_chains_built;
static size_t g_chains_refused;

// --- deterministic scatter -------------------------------------------------

// xorshift rather than rand(): the scatter must be identical across runs, and
// rand() is process-global state that the noise tables and any other seeder
// share.
static unsigned g_rng;

static float rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFFu) / (float)0x1000000u;
}

static float rnd_range(float lo, float hi) {
    return lo + (hi - lo) * rnd();
}

// --- mesh finalisation -----------------------------------------------------

// The one place a generated mesh becomes drawable, because the order is easy to
// get wrong and silent when it is: the chain REWRITES mesh->indices, so building
// it after the upload would send only level 0 and leave every later level's
// offset pointing past the end of the buffer.
//
// mesh_build_lod_chain is called from import.c and nowhere else in the engine,
// so a procedurally generated mesh gets no chain unless an app asks for one.
static void finalize_mesh(Mesh* mesh, Material* material) {
    mesh->material = material;
    int levels = mesh_build_lod_chain(mesh);
    if (levels > 1)
        g_chains_built++;
    else
        g_chains_refused++;
    upload_mesh_buffers_to_gpu(mesh);

    g_distinct_meshes++;
    g_prototype_tris += mesh->index_count / 3u;
}

static void set_node_trs(SceneNode* node, const vec3 pos, float yaw, const vec3 scale) {
    mat4 m;
    glm_mat4_identity(m);
    glm_translate(m, (float*)pos);
    glm_rotate_y(m, yaw, m);
    glm_scale(m, (float*)scale);
    glm_mat4_copy(m, node->original_transform);
}

// One instance of a shared prototype.
//
// `first` consumes the reference create_mesh already handed out; every holder
// after it takes its own, because free_node calls free_mesh on each mesh it
// holds and the refcount is what decides which of those calls actually destroys
// the geometry.
static SceneNode* add_instance(SceneNode* group, Mesh* mesh, bool first, const vec3 pos, float yaw,
                               const vec3 scale) {
    SceneNode* node = create_node();
    add_mesh_to_node(node, first ? mesh : mesh_ref(mesh));
    set_node_trs(node, pos, yaw, scale);
    add_child_node(group, node);
    g_node_count++;
    return node;
}

static SceneNode* make_group(const char* name) {
    SceneNode* g = create_node();
    set_node_name(g, name);
    add_child_node(g_root, g);
    return g;
}

// --- scatter ---------------------------------------------------------------

// One scattered prop, before it becomes a node.
typedef struct Placement {
    vec3 pos;
    vec3 scale;
    float yaw;
    int proto;
    unsigned key; // Morton code of the terrain cell, for spatial ordering
} Placement;

static unsigned part1by1(unsigned n) {
    n &= 0x0000ffffu;
    n = (n | (n << 8)) & 0x00FF00FFu;
    n = (n | (n << 4)) & 0x0F0F0F0Fu;
    n = (n | (n << 2)) & 0x33333333u;
    n = (n | (n << 1)) & 0x55555555u;
    return n;
}

// Z-order rather than row-major. The batcher joins only CONSECUTIVE surviving
// items, so what matters is that props near each other in the world are near
// each other in the list -- a frustum then removes contiguous spans and leaves
// the survivors contiguous too. Row-major would hold only along one axis, and a
// camera looking across the rows would shred every run back to length one.
//
// Randomly ordered scatter measured 2742 instances in 2144 draws. It is the
// single largest thing standing between a scene like this and its batching.
static unsigned morton_key(const TerrainParams* p, float x, float z) {
    float u = (x + p->extent) / (2.0f * p->extent);
    float v = (z + p->extent) / (2.0f * p->extent);
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    unsigned cx = (unsigned)(u * 1023.0f);
    unsigned cz = (unsigned)(v * 1023.0f);
    return part1by1(cx) | (part1by1(cz) << 1);
}

// Prototype first so each one is a contiguous block (a foreign mesh between two
// instances ends the run), then spatially within it.
//
// --no-spatial-sort keeps the prototype grouping and drops only the spatial
// half, which is what isolates the finding: without it the props are still one
// mesh per block and still all batchable, and the ONLY thing that changed is
// whether the survivors of a frustum test end up next to each other.
static int placement_cmp(const void* a, const void* b) {
    const Placement* pa = (const Placement*)a;
    const Placement* pb = (const Placement*)b;
    if (pa->proto != pb->proto)
        return pa->proto < pb->proto ? -1 : 1;
    if (g_args.no_spatial_sort)
        return 0;
    if (pa->key != pb->key)
        return pa->key < pb->key ? -1 : 1;
    return 0;
}

// Emits one group per prototype, in the order the sort already put them.
static void emit_placements(const Placement* items, int count, Mesh* const* protos, int proto_count,
                            const char* prefix) {
    for (int i = 0; i < proto_count; ++i) {
        if (!protos[i])
            continue;
        char name[40];
        snprintf(name, sizeof(name), "%s_%d", prefix, i);
        SceneNode* group = NULL;
        bool first = true;
        for (int k = 0; k < count; ++k) {
            if (items[k].proto != i)
                continue;
            if (!group)
                group = make_group(name);
            add_instance(group, protos[i], first, items[k].pos, items[k].yaw, items[k].scale);
            first = false;
        }
    }
}

// Its own table, not the global one: the texture bake reseeds the shared
// srand-backed generator, and a scatter that shifted depending on whether
// textures were baked first would not be reproducible.
static NoisePerm g_clump;

// Density field, in [0,1]. Without it the scatter is uniform, and a uniform
// scatter of one prototype reads as an orchard however good the tree is --
// clearings and thickets are what make it a forest.
static float clump_density(float x, float z, float freq) {
    float u = (x + g_terrain.extent) * freq;
    float v = (z + g_terrain.extent) * freq;
    float n = noise_perlin3_tiled(&g_clump, u, 3.7f, v, 256);
    n = n * 0.5f + 0.5f;
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

// Rejection sampling against slope and against that density. Returns false when
// a candidate is unusable and the caller should draw another.
static bool sample_ground(float max_slope, float clump_freq, float clump_power, vec3 out_pos) {
    float margin = g_terrain.extent * 0.96f;
    float x = rnd_range(-margin, margin);
    float z = rnd_range(-margin, margin);
    vec3 n;
    terrain_normal_at(&g_terrain, x, z, n);
    if (n[1] < max_slope)
        return false;
    if (clump_freq > 0.0f) {
        float d = clump_density(x, z, clump_freq);
        if (rnd() > powf(d, clump_power))
            return false;
    }
    out_pos[0] = x;
    out_pos[1] = terrain_height_at(&g_terrain, x, z);
    out_pos[2] = z;
    return true;
}

// --- scene construction ----------------------------------------------------

#define BARK_TEX_SIZE 1024
#define LEAF_CELL_SIZE 256

static Texture* bake(Scene* scene, unsigned char* data, int w, int h, int channels, bool srgb,
                     const char* key) {
    if (!data)
        return NULL;
    Texture* t = load_texture_from_memory(scene->tex_pool, key, data, w, h, channels, srgb);
    free(data);
    return t;
}

// Bark and foliage, synthesised rather than loaded -- the app ships no assets.
// Without the leaf atlas's alpha channel the cards render as solid quads, which
// is the difference between a canopy and a cloud of confetti.
static void bake_vegetation_textures(Scene* scene) {
    const int B = BARK_TEX_SIZE;
    float* field = malloc((size_t)B * B * sizeof(float));
    if (field) {
        veg_bark_height_field(field, B, B);
        set_material_albedo_tex(g_mat_bark,
                                bake(scene, veg_bark_albedo(B, B, field), B, B, 3, true,
                                     "forest_bark_albedo"));
        set_material_normal_tex(g_mat_bark, bake(scene, veg_bark_normal(B, B, field), B, B, 3,
                                                 false, "forest_bark_normal"));
        set_material_roughness_tex(g_mat_bark, bake(scene, veg_bark_roughness(B, B, field), B, B, 3,
                                                    false, "forest_bark_rough"));
        free(field);
    }

    const int LW = LEAF_CELL_SIZE * TG_LEAF_VARIANTS;
    const int LH = LEAF_CELL_SIZE;
    unsigned char *la = NULL, *ln = NULL, *lr = NULL;
    veg_leaf_cluster_maps(LW, LH, &la, &ln, &lr);
    set_material_albedo_tex(g_mat_leaf, bake(scene, la, LW, LH, 4, true, "forest_leaf_albedo"));
    set_material_normal_tex(g_mat_leaf, bake(scene, ln, LW, LH, 3, false, "forest_leaf_normal"));
    set_material_roughness_tex(g_mat_leaf, bake(scene, lr, LW, LH, 3, false, "forest_leaf_rough"));
}

static Material* make_material(const char* name, vec3 albedo, float roughness, float metallic) {
    Material* m = create_material();
    m->name = safe_strdup(name);
    glm_vec3_copy(albedo, m->albedo);
    m->roughness = roughness;
    m->metallic = metallic;
    set_material_shader_program(m, g_pbr);
    add_material_to_scene(g_scene, m);
    return m;
}

static void build_terrain(PhysicsWorld* physics, EntityManager* em) {
    SceneNode* group = make_group("terrain");
    for (int tz = 0; tz < g_terrain.tiles; ++tz) {
        for (int tx = 0; tx < g_terrain.tiles; ++tx) {
            Mesh* mesh = create_mesh();
            if (!terrain_build_tile(&g_terrain, tx, tz, mesh)) {
                free_mesh(mesh);
                continue;
            }
            finalize_mesh(mesh, g_mat_terrain);

            SceneNode* node = create_node();
            add_mesh_to_node(node, mesh);
            add_child_node(group, node);
            g_node_count++;
        }
    }

    // Collision. One body over the whole terrain rather than one per tile: the
    // tiles exist for culling and LOD, neither of which physics cares about.
    Mesh* collider = create_mesh();
    if (terrain_build_collider(&g_terrain, COLLIDER_SEGMENTS, collider)) {
        Entity* e = create_entity(em, "terrain");
        entity_set_position(e, (vec3){0.0f, 0.0f, 0.0f});
        PhysicsShapeDesc desc = {.type = SHAPE_MESH, .density = 0.0f};
        desc.mesh.vertices = collider->vertices;
        desc.mesh.vertex_count = collider->vertex_count;
        desc.mesh.indices = collider->indices;
        desc.mesh.index_count = collider->index_count;
        if (!entity_add_rigid_body(e, physics, &desc, MOTION_STATIC, OBJ_LAYER_STATIC))
            fprintf(stderr, "forest: terrain collider rejected\n");
        printf("Terrain collider: %zu triangles\n", collider->index_count / 3u);
    }
    // Never uploaded and never drawn -- Jolt copied the triangles into its own
    // BVH, so this is CPU geometry whose only job is done.
    free_mesh(collider);
}

static void build_trees(void) {
    Mesh* bark[TREE_PROTOTYPES];
    Mesh* leaf[TREE_PROTOTYPES];

    for (int i = 0; i < TREE_PROTOTYPES; ++i) {
        TreeParams tp;
        memset(&tp, 0, sizeof(tp));
        tp.seed = 101 + i * 37;
        // Depth 3 rather than the tree app's 4: a hero tree is 107k triangles,
        // and two thousand of those would be 200M submitted before any culling.
        tp.max_depth = 3;
        tp.trunk_length = 125.0f;
        tp.trunk_radius = 8.0f + (float)(i % 3);
        tp.branches_per_node = 3;
        tp.length_decay = 0.72f;
        tp.taper = 0.62f;
        tp.branch_angle = 34.0f + (float)(i % 3) * 6.0f;
        tp.angle_variance = 12.0f;
        tp.twist = 137.5f;
        tp.droop = 0.28f;
        tp.curve_noise = 0.35f;
        tp.phototropism = 0.45f;
        tp.lateral_density = 0.8f;
        tp.twig_scale = 0.75f;
        tp.show_leaves = 1;
        // The tree app's 15 is a hero-tree size -- a card is a sprig, and at
        // that scale a sprig is two metres across on a twelve-metre tree.
        tp.leaf_size = 9.0f;
        tp.leaf_density = 8.0f;

        TreeSkeleton skel;
        memset(&skel, 0, sizeof(skel));
        tree_skeleton_build(&skel, &tp);

        bark[i] = create_mesh();
        if (!tree_mesh_bark(&skel, &tp, bark[i])) {
            free_mesh(bark[i]);
            bark[i] = NULL;
        } else {
            finalize_mesh(bark[i], g_mat_bark);
        }

        leaf[i] = create_mesh();
        if (!tree_mesh_leaves(&skel, &tp, leaf[i])) {
            free_mesh(leaf[i]);
            leaf[i] = NULL;
        } else {
            finalize_mesh(leaf[i], g_mat_leaf);
        }
        tree_skeleton_free(&skel);
    }

    // Placements drawn once and used for BOTH halves, so a tree's bark and its
    // leaves stand in the same place.
    static Placement items[TREE_COUNT];
    int placed = 0;
    for (int attempt = 0; attempt < TREE_COUNT * 12 && placed < TREE_COUNT; ++attempt) {
        vec3 p = {0.0f, 0.0f, 0.0f}; // out-param; zeroed for static analysis
        // Tight clumps: groves with real clearings between them.
        if (!sample_ground(0.86f, 0.0022f, 2.2f, p))
            continue;
        float s = TREE_WORLD_SCALE * rnd_range(0.65f, 1.6f);
        glm_vec3_copy(p, items[placed].pos);
        glm_vec3_copy((vec3){s, s, s}, items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)TREE_PROTOTYPES) % TREE_PROTOTYPES;
        items[placed].key = morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);

    // Bark and leaves go into SEPARATE group sets. A node holding both would put
    // a leaf item between every pair of bark items, and the run finder breaks on
    // the first item this pass does not want -- so every run would be length one
    // and batching would silently do nothing.
    emit_placements(items, placed, bark, TREE_PROTOTYPES, "bark");
    emit_placements(items, placed, leaf, TREE_PROTOTYPES, "leaf");
    printf("Trees: %d instances over %d prototypes\n", placed, TREE_PROTOTYPES);
}

static void build_rocks(void) {
    Mesh* rocks[ROCK_PROTOTYPES];
    for (int i = 0; i < ROCK_PROTOTYPES; ++i) {
        RockParams rp = rock_default_params();
        rp.seed = 7u + (unsigned)i * 13u;
        rp.subdivisions = 3;
        rp.roughness = 0.26f + 0.06f * (float)(i % 3);
        rp.noise_freq = 1.4f + 0.3f * (float)(i % 4);
        rocks[i] = create_mesh();
        if (!rock_build_mesh(&rp, rocks[i])) {
            free_mesh(rocks[i]);
            rocks[i] = NULL;
            continue;
        }
        finalize_mesh(rocks[i], g_mat_rock);
    }

    static Placement items[ROCK_COUNT];
    int placed = 0;
    for (int attempt = 0; attempt < ROCK_COUNT * 12 && placed < ROCK_COUNT; ++attempt) {
        vec3 p = {0.0f, 0.0f, 0.0f}; // out-param; zeroed for static analysis
        // Looser and at a different frequency, so rock fields do not simply
        // mirror the tree groves.
        if (!sample_ground(0.55f, 0.0035f, 1.3f, p))
            continue;
        // Non-uniform scale on purpose: it sits the rock into the ground and,
        // less decoratively, makes each instance's normal matrix differ from
        // mat3(model) -- the lane spec 11.28's fixture was blind to until its
        // props stopped being translation-only boxes.
        float s = rnd_range(0.9f, 5.5f);
        // Sunk a little, but not squashed: a y-scale near half reads as a disc
        // lying on the ground rather than as a boulder sitting in it.
        p[1] -= s * 0.22f;
        glm_vec3_copy(p, items[placed].pos);
        glm_vec3_copy((vec3){s * rnd_range(0.85f, 1.25f), s * rnd_range(0.7f, 1.05f),
                             s * rnd_range(0.85f, 1.25f)},
                      items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)ROCK_PROTOTYPES) % ROCK_PROTOTYPES;
        items[placed].key = morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);
    emit_placements(items, placed, rocks, ROCK_PROTOTYPES, "rock");
    printf("Rocks: %d instances over %d prototypes\n", placed, ROCK_PROTOTYPES);
}

// The lighting when --no-sky takes the atmosphere away. Not merely "skip the
// sky": the sky IS this scene's light -- it supplies the sun, the IBL and the
// aerial perspective -- so removing it without a replacement renders black.
//
// A single directional caster with a flat ambient, which is enough to keep the
// geometry readable and the shadow pass working. It looks worse than the real
// thing on purpose; the flag exists to take the atmosphere's cost out of a
// measurement, not to offer a second look.
static void build_fallback_sun(void) {
    Light* sun = create_light();
    set_light_name(sun, "sun");
    set_light_type(sun, LIGHT_DIRECTIONAL);
    set_light_direction(sun, (vec3){-0.45f, -0.78f, -0.44f});
    set_light_color(sun, (vec3){1.0f, 0.96f, 0.88f});
    set_light_intensity(sun, 3.2f);
    set_light_cast_shadows(sun, true);
    set_light_size(sun, 4.0f, 4.0f);
    add_light_to_scene(g_scene, sun);

    SceneNode* node = create_node();
    set_node_name(node, "sun");
    set_node_light(node, sun);
    add_child_node(g_root, node);

    // No IBL without the sky, so this uniform term is the whole of the fill.
    // It defaults to zero, which would leave every shadowed surface black.
    // Cool, because the thing it stands in for is skylight.
    glm_vec3_copy((vec3){0.45f, 0.55f, 0.75f}, g_scene->ambient_radiance);
    g_scene->render_skybox = false;
}

static void build_sky_and_sun(Engine* engine) {
    SkyAtmosphere* sky = create_sky_atmosphere();
    IBLResources* ibl = create_ibl_resources();
    if (!sky || !ibl)
        return;

    // The CLI wins, and must land before the bake: the LUTs are a function of
    // where the sun is.
    // Low on purpose. A high sun lights the fog uniformly from above and the
    // medium reads as haze; a low one rakes through it, so the anisotropy has a
    // direction to work along and the ridge lines separate into layers.
    sky->sun_elevation_deg = g_args.sun_elevation > -900.0f ? g_args.sun_elevation : 15.0f;
    sky->sun_azimuth_deg = g_args.sun_azimuth > -900.0f ? g_args.sun_azimuth : 152.0f;
    // One world unit is one metre here, which is what makes aerial perspective
    // read correctly over a kilometre instead of hazing the near ground.
    sky->world_units_per_km = 1000.0f;
    sky->aerial_enabled = !g_args.no_aerial;
    sky_update_sun_dir(sky);

    if (sky_bake_static_luts(sky, engine) != 0 || sky_bake(sky, ibl, engine) != 0)
        return;

    g_scene->sky = sky;
    g_scene->ibl = ibl;
    g_scene->render_skybox = true;
    g_scene->skybox_brightness = 1.0f;
    g_scene->skybox_ground_projection = false;

    Light* sun = create_light();
    set_light_name(sun, "sun");
    set_light_type(sun, LIGHT_DIRECTIONAL);
    set_light_cast_shadows(sun, true);
    set_light_size(sun, 4.0f, 4.0f);
    sky->sun_light = sun;
    // 3, not the tree app's 10. That app frames one subject against a backdrop
    // and can afford to blow the highlights; here every lit surface facing a
    // 44-degree sun clips at 9, which turned bare rock white and left everything
    // in shadow crushed -- a dynamic-range problem that reads as a material bug.
    sky->sun_base_intensity = 6.0f;
    sky_apply_sun_to_light(sky);
    add_light_to_scene(g_scene, sun);

    SceneNode* node = create_node();
    set_node_name(node, "sun");
    set_node_light(node, sun);
    add_child_node(g_root, node);
}

// --- callbacks -------------------------------------------------------------

static void on_init(Game* game) {
    Engine* engine = game->engine;
    g_pbr = get_engine_shader_program_by_name(engine, "pbr");

    g_scene = create_scene();
    g_root = create_node();
    set_node_name(g_root, "root");
    set_scene_root_node(g_scene, g_root);
    game_set_scene(game, g_scene);

    g_terrain = terrain_default_params();
    g_terrain.seed = g_args.seed;
    // Taller and broader than the library default: at 55 over a 250-unit
    // wavelength the ground reads as flat from anywhere a person stands, and
    // terrain that reads flat gives LOD and culling nothing to work with.
    g_terrain.height = 95.0f;
    g_terrain.base_freq = 0.0026f;
    g_terrain.octaves = 6;
    g_rng = g_args.seed * 2654435761u + 1u;
    noise_perm_init(&g_clump, g_args.seed ^ 0x5bf03635u);

    // Albedo white on the textured materials: the shader multiplies factor by
    // map, so any factor below one darkens the whole range.
    g_mat_terrain = make_material("terrain", (vec3){1.0f, 1.0f, 1.0f}, 0.92f, 0.0f);
    g_mat_bark = make_material("bark", (vec3){1.0f, 1.0f, 1.0f}, 1.0f, 0.0f);
    g_mat_leaf = make_material("leaf", (vec3){0.85f, 1.0f, 0.8f}, 1.0f, 0.0f);
    // Cutout, two-sided, and allowed into the shadow map -- foliage_shadows is
    // what dapples the ground, and without alpha_mode the cards are solid quads.
    g_mat_leaf->alpha_mode = ALPHA_MASK;
    g_mat_leaf->alphaCutoff = 0.4f;
    g_mat_leaf->doubleSided = true;
    g_mat_leaf->foliage_shadows = true;
    // Thin leaves transmit; without it a backlit canopy reads as opaque plastic.
    g_mat_leaf->subsurface = 0.55f;
    // Dark wet stone. Rock is the only light NEUTRAL surface in a scene of dark
    // foliage, so anything near a realistic granite albedo reads as white against
    // it however the lighting is balanced -- the surrounding palette sets what
    // this can be, not the material on its own.
    g_mat_rock = make_material("rock", (vec3){0.085f, 0.082f, 0.078f}, 0.88f, 0.0f);

    bake_vegetation_textures(g_scene);

    PhysicsConfig pc = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&pc);
    game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    build_terrain(physics, em);
    build_trees();
    build_rocks();
    if (g_args.no_sky)
        build_fallback_sun();
    else
        build_sky_and_sun(engine);

    // Standing on the surface, not dropped onto it. The capsule's origin sits
    // half_height + radius above whatever it rests on, so spawning any higher
    // means the first second of every run is a fall -- which reads as the
    // character sliding before it settles.
    const float capsule_rest = 0.9f + 0.4f;
    float spawn_y = terrain_height_at(&g_terrain, 0.0f, 0.0f) + capsule_rest + 0.02f;
    g_player = create_entity(em, "player");
    entity_set_position(g_player, (vec3){0.0f, spawn_y, 0.0f});
    CharacterControllerConfig cc = character_controller_default_config();
    cc.capsule_radius = 0.4f;
    cc.capsule_half_height = 0.9f;
    cc.step_height = 0.5f;
    // 45, not 55: terrain this steep is common, and a controller willing to
    // stand on a 55-degree face spends its time creeping down one.
    cc.max_slope_angle = 45.0f;
    // Longer than the 0.5 default, because the ground here is a triangle mesh
    // and a capsule crossing a convex edge otherwise leaves it for a frame,
    // loses ground contact, and starts falling mid-stride.
    cc.stick_to_floor_distance = 1.2f;
    cc.penetration_recovery_speed = 1.5f;
    entity_add_character_controller(g_player, physics, &cc);

    physics_world_optimize(physics);

    // Shadows sized to the terrain, not left at the library defaults (ortho 2000
    // and a single origin-centred map). The outermost cascade is fitted around a
    // hardcoded origin, which is why the terrain is centred there.
    ShadowSystem* ss = g_scene->shadow_system;
    if (ss) {
        ss->enabled = true;
        ss->ortho_size = g_terrain.extent;
        ss->near_plane = 0.5f;
        ss->far_plane = g_terrain.extent * 6.0f;
        ss->cascade_count = SHADOW_CASCADES;
        ss->pcss_enabled = true;
        ss->pcss_softness = 1.2f;
    }

    Camera* camera = create_camera();
    // 0.5 / 2000 rather than the 0.1 / 1000 default: a kilometre of terrain
    // needs the far plane, and 0.1 near against it is a 20000:1 depth ratio that
    // z-fights across the whole distance.
    set_camera_perspective(camera, glm_rad(58.0f), 0.5f, 2000.0f);
    set_camera_position(camera, (vec3){0.0f, spawn_y + 6.0f, 16.0f});
    set_camera_look_at(camera, (vec3){0.0f, spawn_y, 0.0f});
    set_camera_up_vector(camera, (vec3){0.0f, 1.0f, 0.0f});
    set_engine_camera(engine, camera);
    set_engine_camera_mode(engine, CAMERA_MODE_FREE);

    // Exposure pinned rather than adaptive. Two reasons, and the second is the
    // one that matters: AGENTS.md names auto-exposure as the top determinism
    // hazard for anything compared across builds, and every arm here reads a
    // frame or a counter from this app. The first is that a scene of dark
    // foliage makes the meter boost until the brightest surface -- bare rock --
    // clips to white, which is what sent me looking for a bug in the rock
    // material that was never there.
    engine->exposure.automatic = false;
    // Picked by measuring, not by eye: at 1.0 the lit ground averages 72/255,
    // which reads as dusk. 1.8 puts it near 120 with nothing clipping.
    engine->exposure.multiplier = 1.8f;

    if (g_args.render_mode > 0)
        engine->current_render_mode = (RenderMode)g_args.render_mode;

    if (g_args.no_lod)
        engine->lod_enabled = false;
    if (g_args.no_instancing)
        engine->instancing_enabled = false;
    if (g_args.lod_bias > 0.0f)
        engine->lod_bias = g_args.lod_bias;

    // Volumetric fog, on by default, and sized to the world rather than left at
    // the struct defaults -- fog_far bounds the froxel volume, so a value short
    // of the far plane simply stops the medium partway across the terrain and
    // leaves a visible edge where it ends.
    //
    // Scale matters more than density here: one unit is one metre, so the
    // extinction is per metre and the numbers are small.
    if (engine->postfx && !g_args.no_fog) {
        PostFX* fx = engine->postfx;
        fx->fog_enabled = true;
        fx->fog_density = 0.0022f;
        fx->fog_height_falloff = 42.0f; // thick in the valleys, thin off the ridges
        fx->fog_floor_y = -30.0f;
        fx->fog_near = 0.0f; // derive from fog_far
        fx->fog_far = 1400.0f;
        // Forward-scattering, so looking toward the sun lights the medium up and
        // looking away leaves it flat -- most of what reads as "moody".
        fx->fog_anisotropy = 0.72f;
        fx->fog_sun_boost = 2.2f;
        glm_vec3_copy((vec3){0.030f, 0.038f, 0.055f}, fx->fog_ambient);
    }

    set_engine_show_gui(engine, !engine->headless);
    set_engine_show_fps(engine, !engine->headless);

    printf("Forest: %zu distinct meshes, %zu prototype triangles, %zu nodes\n", g_distinct_meshes,
           g_prototype_tris, g_node_count);
    printf("Forest: %zu LOD chains built, %zu refused\n", g_chains_built, g_chains_refused);
}

static void on_update(Game* game, double dt) {
    if (!g_player)
        return;
    CharacterController* cc = entity_get_character_controller(g_player);
    if (!cc)
        return;

    vec3 input_dir;
    input_wasd_direction(&game->input, input_dir);

    // Rotated into the camera's yaw. gametest's is world-axis-aligned, which
    // stops making sense the moment the camera is not facing -Z.
    vec3 fwd = {sinf(g_cam_yaw), 0.0f, cosf(g_cam_yaw)};
    vec3 right = {-cosf(g_cam_yaw), 0.0f, sinf(g_cam_yaw)};

    vec3 vel;
    character_controller_get_velocity(cc, vel);

    const float speed = input_key_down(&game->input, GLFW_KEY_LEFT_SHIFT) ? 16.0f : 7.0f;
    vec3 move = {0.0f, 0.0f, 0.0f};
    glm_vec3_muladds(fwd, -input_dir[2] * speed, move);
    glm_vec3_muladds(right, input_dir[0] * speed, move);

    bool grounded = character_controller_is_grounded(cc);

    // Horizontal velocity is SET, not accumulated, so releasing the keys stops
    // the character rather than coasting. On a slope that is not enough on its
    // own -- see the vertical term below, which is what actually caused the
    // skating.
    vel[0] = move[0];
    vel[2] = move[2];

    if (grounded) {
        // Zero, not a small negative. Any downward velocity left on a grounded
        // character is resolved by ExtendedUpdate ALONG the surface, so on a
        // slope it becomes downhill travel: measured at 0.36 m/s on a 10-degree
        // face from a -2 m/s residual, which is 2*sin(10) and exactly the
        // skating this was meant to cure. Ground adherence is
        // stick_to_floor_distance's job, not a velocity's.
        vel[1] = 0.0f;
    } else {
        vel[1] -= 22.0f * (float)dt;
    }

    if (input_key_pressed(&game->input, GLFW_KEY_SPACE) && grounded)
        vel[1] = 9.5f;

    character_controller_set_velocity(cc, vel);

    // Whether the character is at rest is not something the frame shows -- the
    // camera follows it, so drift and stillness look identical from inside.
    if (g_args.trace_player) {
        static int step;
        if (step++ % 30 == 0) {
            vec3 pos, gn;
            character_controller_get_position(cc, pos);
            character_controller_get_ground_normal(cc, gn);
            printf("player t=%5.2f pos %8.2f %8.2f %8.2f  vel %6.2f %6.2f %6.2f  "
                   "grounded %d  ground_n.y %.3f  terrain %.2f\n",
                   (double)step / 60.0, (double)pos[0], (double)pos[1], (double)pos[2],
                   (double)vel[0], (double)vel[1], (double)vel[2], grounded ? 1 : 0, (double)gn[1],
                   (double)terrain_height_at(&g_terrain, pos[0], pos[2]));
        }
    }
}

static void on_render(Game* game, double alpha) {
    (void)alpha;
    Engine* engine = game->engine;
    if (!g_scene || !g_scene->root_node)
        return;

    Camera* camera = engine->camera;
    if (camera) {
        if (g_args.cam_set) {
            set_camera_position(camera, g_args.cam_eye);
            set_camera_look_at(camera, g_args.cam_target);
        } else if (g_player) {
            if (input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_LEFT) ||
                input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_RIGHT)) {
                double dx = 0.0, dy = 0.0;
                input_mouse_delta(&game->input, &dx, &dy);
                g_cam_yaw -= (float)dx * 0.005f;
                g_cam_pitch += (float)dy * 0.005f;
            }
            if (input_key_down(&game->input, GLFW_KEY_Q))
                g_cam_yaw += 0.03f;
            if (input_key_down(&game->input, GLFW_KEY_E))
                g_cam_yaw -= 0.03f;
            if (g_cam_pitch < -0.2f)
                g_cam_pitch = -0.2f;
            if (g_cam_pitch > 1.25f)
                g_cam_pitch = 1.25f;

            vec3 target;
            glm_vec3_copy(g_player->position, target);
            target[1] += 1.5f;

            float ch = cosf(g_cam_pitch);
            vec3 eye = {target[0] - sinf(g_cam_yaw) * ch * g_cam_dist,
                        target[1] + sinf(g_cam_pitch) * g_cam_dist,
                        target[2] - cosf(g_cam_yaw) * ch * g_cam_dist};
            // Never below the ground the character is standing on.
            float floor_y = terrain_height_at(&g_terrain, eye[0], eye[2]) + 1.0f;
            if (eye[1] < floor_y)
                eye[1] = floor_y;
            set_camera_position(camera, eye);
            set_camera_look_at(camera, target);
        }
        // Without both of these the view matrix keeps whatever it had; nothing
        // else in a game-framework app writes it.
        update_engine_camera_lookat(engine);
        update_engine_camera_perspective(engine);
    }

    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(g_scene->root_node, engine->model_matrix);

    render_current_scene(engine);
}

static void on_shutdown(Game* game) {
    // run_game does not report; the render app does this at its own exit. Here
    // because the whole app exists to be read off these tables.
    if (game && game->engine)
        profiler_report(game->engine->profiler);
}

// --- entry point -----------------------------------------------------------

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "  -x, --headless          Hidden window (capture / CI)\n");
    fprintf(stderr, "  -f, --frames N          Exit after N frames\n");
    fprintf(stderr, "  -S, --screenshot PATH   Save the final frame as PPM\n");
    fprintf(stderr, "  -W, -H <n>              Window size\n");
    fprintf(stderr, "      --profiler          Per-pass timing + submission counters\n");
    fprintf(stderr, "      --no-lod            Draw every mesh at LOD level 0\n");
    fprintf(stderr, "      --no-instancing     One draw per mesh\n");
    fprintf(stderr, "      --no-sky            Plain directional rig, no atmosphere\n");
    fprintf(stderr, "      --no-fog            Disable the volumetric fog\n");
    fprintf(stderr, "      --no-aerial         Keep the sky, drop aerial perspective\n");
    fprintf(stderr, "      --sun-elevation <d> Sun elevation in degrees\n");
    fprintf(stderr, "      --sun-azimuth <d>   Sun azimuth in degrees\n");
    fprintf(stderr, "      --no-spatial-sort   Scatter without Morton ordering\n");
    fprintf(stderr, "      --render-mode N     1 = normals, 6 = albedo\n");
    fprintf(stderr, "      --lod-bias F        >1 holds detail longer\n");
    fprintf(stderr, "      --seed N            Terrain and scatter seed\n");
    fprintf(stderr, "      --cam-eye x,y,z     Pin the camera (disables follow)\n");
    fprintf(stderr, "      --cam-target x,y,z  Pinned camera aim point\n");
}

static bool parse_vec3(const char* s, vec3 out) {
    return sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

int main(int argc, char** argv) {
    memset(&g_args, 0, sizeof(g_args));
    g_args.seed = 1337u;
    g_args.sun_elevation = -1000.0f; // sentinel: keep the app's own angle
    g_args.sun_azimuth = -1000.0f;
    int cam_eye_set = 0, cam_target_set = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-x") || !strcmp(a, "--headless")) {
            g_args.headless = 1;
        } else if ((!strcmp(a, "-f") || !strcmp(a, "--frames")) && i + 1 < argc) {
            g_args.frames = atoi(argv[++i]);
        } else if ((!strcmp(a, "-S") || !strcmp(a, "--screenshot")) && i + 1 < argc) {
            g_args.screenshot = argv[++i];
        } else if (!strcmp(a, "--profiler")) {
            g_args.profiler = 1;
        } else if (!strcmp(a, "--no-lod")) {
            g_args.no_lod = 1;
        } else if (!strcmp(a, "--no-instancing")) {
            g_args.no_instancing = 1;
        } else if (!strcmp(a, "--lod-bias") && i + 1 < argc) {
            g_args.lod_bias = strtof(argv[++i], NULL);
        } else if ((!strcmp(a, "-W") || !strcmp(a, "--width")) && i + 1 < argc) {
            g_args.width = atoi(argv[++i]);
        } else if ((!strcmp(a, "-H") || !strcmp(a, "--height")) && i + 1 < argc) {
            g_args.height = atoi(argv[++i]);
        } else if (!strcmp(a, "--no-sky")) {
            g_args.no_sky = 1;
        } else if (!strcmp(a, "--no-fog")) {
            g_args.no_fog = 1;
        } else if (!strcmp(a, "--trace-player")) {
            g_args.trace_player = 1;
        } else if (!strcmp(a, "--no-aerial")) {
            g_args.no_aerial = 1;
        } else if (!strcmp(a, "--sun-elevation") && i + 1 < argc) {
            g_args.sun_elevation = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--sun-azimuth") && i + 1 < argc) {
            g_args.sun_azimuth = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--no-spatial-sort")) {
            g_args.no_spatial_sort = 1;
        } else if (!strcmp(a, "--render-mode") && i + 1 < argc) {
            g_args.render_mode = atoi(argv[++i]);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            g_args.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--cam-eye") && i + 1 < argc) {
            cam_eye_set = parse_vec3(argv[++i], g_args.cam_eye);
        } else if (!strcmp(a, "--cam-target") && i + 1 < argc) {
            cam_target_set = parse_vec3(argv[++i], g_args.cam_target);
        } else {
            print_usage(argv[0]);
            return a[0] == '-' ? 1 : 0;
        }
    }
    // Both or neither: half a camera would silently aim at the origin.
    g_args.cam_set = cam_eye_set && cam_target_set;

    GameConfig config = game_default_config();
    config.title = "Cetra Forest";
    config.width = g_args.width > 0 ? g_args.width : 1600;
    config.height = g_args.height > 0 ? g_args.height : 900;
    config.headless = g_args.headless != 0;
    config.exit_after_frames = g_args.frames;
    config.screenshot_path = g_args.screenshot;
    config.profiler = g_args.profiler != 0;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "forest: failed to create game\n");
        return 1;
    }

    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    run_game(game);
    free_game(game);
    return 0;
}
