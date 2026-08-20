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
 *   - wind on the trees, which spec 11.53 made possible: a wind material used
 *     to be exempt from frustum culling entirely, and 2,000 uncullable trees
 *     defeat the point of the app
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
#include "cetra/water.h"
#include "cetra/wind.h"
#include "cetra/transform.h"
#include "cetra/util.h"

#include "cetra/game/character.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

#include "cetra/procedural/erosion.h"
#include "cetra/procedural/heightmap.h"
#include "cetra/procedural/rock.h"
#include "cetra/procedural/terrain.h"
#include "cetra/procedural/terrain_tex.h"
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
    int no_sort_opaque;   // opaque front-to-back ordering is on by default
    int depth_prepass;    // position-only depth before shading; off by default
    int force_taa;        // TAA headless too; diagnostic, costs determinism
    int msaa;             // requested sample count; 0 = the TAA/headless policy decides
    int headless_jitter;  // sub-pixel jitter headless; TAA is inert without it
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
    int water;         // flood the terrain to --water-level (spec 11.32)
    float water_level; // world Y of the still surface
    int erode;         // bake a heightfield and run erosion over it (spec 11.59)
    int erode_res;     // field resolution; 0 = EROSION_DEFAULT_RES
    int erode_iterations;
    int erode_workers; // 0 = size from the machine
    int erode_probe;   // print the bake's own numbers; implies --erode
    const char* erode_save;  // write the baked field here as .r16
    const char* heightmap;   // load a field from here instead of baking
    float heightmap_min;     // world Y the file's 0 maps to
    float heightmap_max;     // world Y the file's 65535 maps to
    int height_probe;        // print sampled heights, normals and masks
    int scatter_probe;       // print where the scatter put things, against the drainage
    // Place the whole world this far from the origin on X and Z (spec 11.62).
    // The instrument fp32's relative precision is measured with: at 0 this is
    // the frame the app has always rendered.
    float world_offset;
    // Diagnostic (--origin-shift-at): re-centre the world on the camera at this
    // frame, 0 = never. A shift is a TRANSITION, and a transition is invisible to
    // every headless arm until something can ask for one at a known frame -- the
    // same reason --shadows-off-at exists rather than being reachable by a state
    // flag. What it buys is an exact test: the shift changes only where
    // coordinates are measured from, so the frames after it must be identical to
    // a run that never shifted.
    int origin_shift_at;
    // Camera drift the engine tolerates before re-centring on its own, 0 = never.
    // The automatic half of the same mechanism --origin-shift-at drives by hand.
    float origin_shift_distance;
} ForestArgs;

static ForestArgs g_args;

static TerrainParams g_terrain;
// The eroded field, when --erode is on. File-static because g_terrain borrows it
// for the process's lifetime -- the terrain is built once and never rebuilt.
static TerrainField g_field;
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
static const float CAM_DISTANCE = 14.0f;

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

// Half-open [0, 1): the numerator is masked to 24 bits and the divisor is 2^24,
// so 1.0 is not reachable. Callers index prototype arrays with (int)(rnd() * N)
// and rely on that.
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

// The one place a generated mesh becomes drawable. The chain REWRITES
// mesh->indices, so it has to precede the upload -- doing it after sends level 0
// and leaves every later level's offset pointing past the end of the buffer.
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

// One instance of a shared prototype. Every holder takes its own reference; the
// creator drops the one create_mesh handed out after emitting, so the plain rule
// holds -- whoever creates it releases it -- and a prototype that ends up with
// no instances is freed rather than leaked.
static void add_instance(SceneNode* group, Mesh* mesh, const vec3 pos, float yaw,
                         const vec3 scale) {
    SceneNode* node = create_node();
    add_mesh_to_node(node, mesh_ref(mesh));
    set_node_trs(node, pos, yaw, scale);
    add_child_node(group, node);
    g_node_count++;
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
// It is the single largest thing standing between a scene like this and its
// batching; --no-spatial-sort exists so the difference can be measured rather
// than asserted, and specs/11.29 records the figures.
static unsigned morton_key(const TerrainParams* p, float x, float z) {
    float dx, dz;
    terrain_to_domain(p, x, z, &dx, &dz);
    float u = dx / (2.0f * p->extent);
    float v = dz / (2.0f * p->extent);
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    unsigned cx = (unsigned)(u * 1023.0f);
    unsigned cz = (unsigned)(v * 1023.0f);
    return part1by1(cx) | (part1by1(cz) << 1);
}

// Prototype first so each one is a contiguous block (a foreign mesh between two
// instances ends the run), then spatially within it.
//
// A pure function of its two arguments. --no-spatial-sort is honoured in the
// KEY (zeroed at fill time) rather than here, which produces the same comparator
// outputs over the same array and keeps the hidden global out of a comparator.
//
// Prototype grouping does not depend on this: emit_placements filters by
// prototype, so each one is contiguous whatever order the array is in. Sorting
// on it anyway keeps the two agreeing about block order.
static int placement_cmp(const void* a, const void* b) {
    const Placement* pa = (const Placement*)a;
    const Placement* pb = (const Placement*)b;
    if (pa->proto != pb->proto)
        return pa->proto < pb->proto ? -1 : 1;
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
        for (int k = 0; k < count; ++k) {
            if (items[k].proto != i)
                continue;
            if (!group)
                group = make_group(name);
            add_instance(group, protos[i], items[k].pos, items[k].yaw, items[k].scale);
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
    float dx, dz;
    terrain_to_domain(&g_terrain, x, z, &dx, &dz);
    float u = dx * freq;
    float v = dz * freq;
    float n = noise_perlin3_tiled(&g_clump, u, 3.7f, v, 256);
    n = n * 0.5f + 0.5f;
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

// The drainage a tree tolerates standing in: the MIDPOINT of the band over which
// terrain_bake_splat turns the ground into a channel bed, i.e. where gravel
// stops being a tint and starts being what the surface is made of.
//
// Derived from the band rather than written down beside it. The first version
// was a literal 0.52 whose comment claimed it sat "below the gravel band's low
// edge... rejecting a few per cent of the map" -- and --scatter-probe measured
// it rejecting 43.9%, because flow's mean is near 0.49 and 0.52 is barely above
// it. A number that has to agree with another module's calibration should be
// computed from it.
#define TREE_MAX_FLOW ((TERRAIN_CHANNEL_FLOW_LO + TERRAIN_CHANNEL_FLOW_HI) * 0.5f)

/*
 * Where the scatter put things, against the drainage it was placing into.
 *
 * The instrument exists because the defect it guards is invisible in a frame: a
 * tree standing in a stream bed looks exactly like a tree, and the only thing
 * wrong with it is a number nothing prints. It reports the terrain's OWN flow
 * range beside the trees', which is what stops the arm being vacuous -- with no
 * erosion bake every mask reads zero, the placement rule is trivially satisfied,
 * and an arm checking only the trees would pass against a build that had never
 * implemented any of this.
 */
static void scatter_probe(const Placement* items, int count) {
    float tree_max = 0.0f, tree_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float f = terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, items[i].pos[0], items[i].pos[2]);
        tree_sum += f;
        if (f > tree_max)
            tree_max = f;
    }

    // The terrain's own distribution, on a coarse grid over the same margin the
    // scatter draws from -- sampling the whole extent would include an edge the
    // scatter never reaches and report a range no tree could have hit.
    //
    // The FRACTION over the limit is the number that matters, not the peak.
    // erosion.c normalises flow to its own maximum, so a peak near 1.0 is true of
    // any sim that ran at all; what says candidates existed to reject is how much
    // of the domain the rule actually excludes.
    const int G = 64;
    float land_sum = 0.0f;
    int land_over = 0;
    float margin = g_terrain.extent * 0.96f;
    for (int j = 0; j < G; j++) {
        for (int i = 0; i < G; i++) {
            float x = terrain_world_x(&g_terrain, terrain_field_node(margin, G, i));
            float z = terrain_world_z(&g_terrain, terrain_field_node(margin, G, j));
            float f = terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, x, z);
            land_sum += f;
            if (f > TREE_MAX_FLOW)
                land_over++;
        }
    }

    printf("scatter-probe trees=%d tree_flow_max=%.4f tree_flow_mean=%.4f "
           "land_flow_mean=%.4f land_frac_over=%.4f limit=%.4f\n",
           count, (double)tree_max, count ? (double)(tree_sum / (float)count) : 0.0,
           (double)(land_sum / (float)(G * G)), (double)land_over / (double)(G * G),
           (double)TREE_MAX_FLOW);
}

// Rejection sampling against slope, against drainage, and against that density.
// Returns false when a candidate is unusable and the caller should draw another.
//
// `max_flow` is the drainage a prop tolerates standing in, and it is the half
// that was missing until spec 11.60. The ground already knew where its streams
// were -- terrain_bake_splat paints them gravel -- while the scatter placed by
// SLOPE alone, and a channel bed is flat, so it passed the slope gate with room
// to spare and the frame grew trees down the middle of a watercourse. Scoured
// bedrock had the same problem in reverse: the splat paints it rock precisely
// where the surface is shallow enough to plant on.
//
// 1.0 means "anywhere", which is right for a boulder: a rock in a stream bed is
// where rocks actually are.
static bool sample_ground(float max_slope, float max_flow, float clump_freq, float clump_power,
                          vec3 out_pos) {
    float margin = g_terrain.extent * 0.96f;
    float x = terrain_world_x(&g_terrain, rnd_range(-margin, margin));
    float z = terrain_world_z(&g_terrain, rnd_range(-margin, margin));
    vec3 n = {0.0f, 1.0f, 0.0f};
    terrain_normal_at(&g_terrain, x, z, n);
    if (n[1] < max_slope)
        return false;
    // Reads 0 with no erosion bake, so this degrades to exactly the pre-11.60
    // placement rather than to a special case -- the same property that lets
    // terrain_tint blend by the masks unconditionally.
    if (max_flow < 1.0f && terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, x, z) > max_flow)
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

// One layer map is this square, and the number is NOT a memory decision even
// though it looks like one.
//
// Every tenant of the material texture array is resampled to one canonical size,
// which is the largest dimension of any source in the scene -- and in this app
// that is the leaf atlas, 8 cells of 256 laid out along u, so 2048. The array
// therefore costs 234.7 MB whatever this says, and a 512 map was being upsampled
// four times over in each axis to fill a layer it had no detail for. Measured
// both ways: 234.7 MB at 512 and at 1024, +1.7 s of bake in a DEBUG build (the
// preset build.sh defaults to, which passes no -O at all).
//
// So this buys detail that is already paid for. Raising it further is free in
// VRAM too, up to 2048, and stops being free the moment something else in the
// scene is larger than the leaf atlas.
#define TERRAIN_LAYER_TEX_SIZE 1024

// The splat is baked at the erosion field's own resolution when there is one, so
// it is an exact resample of the masks rather than an interpolation of an
// interpolation. With no field the masks read zero everywhere and this degrades
// to slope alone, which is the un-eroded look and is the point.
#define TERRAIN_SPLAT_FALLBACK_RES 512

// The four grounds, in splat order: layer 0 takes the remainder, then r, g, b.
// terrain_bake_splat decides which mask feeds which channel; this decides what
// each channel LOOKS like, which is the half an app gets to choose.
//
// One table rather than two parallel arrays and a ternary. `uv_scale` is world
// units per tile, and it belongs beside the kind for the same reason the kinds
// are ordered here: a coarse ground repeats over a longer distance than a fine
// one, which is a fact about that ground and not about the loop that bakes it.
static const struct {
    TerrainLayerKind kind;
    const char* name;
    float uv_scale;
} TERRAIN_LAYERS[] = {
    {TERRAIN_LAYER_GRASS, "grass", 4.0f},
    {TERRAIN_LAYER_ROCK, "rock", 6.0f},
    {TERRAIN_LAYER_SILT, "silt", 4.0f},
    {TERRAIN_LAYER_GRAVEL, "gravel", 3.0f},
};
_Static_assert(sizeof(TERRAIN_LAYERS) / sizeof(TERRAIN_LAYERS[0]) <= MATERIAL_MAX_LAYERS,
               "a fifth ground would arm a layer count the shader silently clamps away");

// The ground, as a layered surface (spec 11.60). Replaces a white albedo times a
// per-vertex tint at 2.6 units, which is why the terrain did not hold up at
// walking distance whatever the palette was.
static void bake_terrain_layers(Scene* scene) {
    const int T = TERRAIN_LAYER_TEX_SIZE;
    int count = (int)(sizeof(TERRAIN_LAYERS) / sizeof(TERRAIN_LAYERS[0]));
    for (int i = 0; i < count; i++) {
        unsigned char *albedo = NULL, *surface = NULL;
        terrain_layer_maps(TERRAIN_LAYERS[i].kind, T, g_args.seed + (unsigned)i * 977u, &albedo,
                           &surface);
        char key[64];
        snprintf(key, sizeof(key), "forest_layer_%s_a", TERRAIN_LAYERS[i].name);
        set_material_layer_albedo_tex(
            g_mat_terrain, i,
            load_texture_from_memory_owned(scene->tex_pool, key, albedo, T, T, 4, false));
        snprintf(key, sizeof(key), "forest_layer_%s_s", TERRAIN_LAYERS[i].name);
        set_material_layer_surface_tex(
            g_mat_terrain, i,
            load_texture_from_memory_owned(scene->tex_pool, key, surface, T, T, 4, false));
        g_mat_terrain->layers[i].uv_scale = TERRAIN_LAYERS[i].uv_scale;
    }

    int res = g_terrain.field ? g_terrain.field->res : TERRAIN_SPLAT_FALLBACK_RES;
    unsigned char* splat = malloc((size_t)res * (size_t)res * 3u);
    if (splat && terrain_bake_splat(&g_terrain, res, splat)) {
        set_material_splat_tex(g_mat_terrain,
                               load_texture_from_memory_owned(scene->tex_pool,
                                                              "forest_terrain_splat", splat, res,
                                                              res, 3, false));
    } else {
        free(splat);
        fprintf(stderr, "forest: splat bake failed; the ground falls back to layer 0\n");
    }

    // The splat is a function of world XZ over the terrain's own square, which
    // is what makes it work at all here: terrain tiles carry no UV1 (build_grid
    // writes a literal zero), so a mesh-local reading samples one texel and the
    // whole kilometre resolves to layer 0.
    g_mat_terrain->splat_space = SPLAT_SPACE_WORLD_XZ;
    g_mat_terrain->splat_origin[0] = terrain_world_x(&g_terrain, -g_terrain.extent);
    g_mat_terrain->splat_origin[1] = terrain_world_z(&g_terrain, -g_terrain.extent);
    g_mat_terrain->splat_size[0] = g_mat_terrain->splat_size[1] = 2.0f * g_terrain.extent;

    // Last, because it is what arms the shader.
    g_mat_terrain->layer_count = count;
    // And the mesh side of the same switch, which must be set before
    // build_terrain writes a single vertex colour: the layers carry the ground's
    // colour now, so the tint has to become macro variation or the two multiply
    // and the ground comes out near black.
    g_terrain.layered = true;
    printf("Terrain layers: %d at %dx%d, splat %dx%d\n", count, T, T, res, res);
}

// Bark and foliage, synthesised rather than loaded -- the app ships no assets.
// Without the leaf atlas's alpha channel the cards render as solid quads, which
// is the difference between a canopy and a cloud of confetti.
static void bake_vegetation_textures(Scene* scene) {
    const int B = BARK_TEX_SIZE;
    float* field = malloc((size_t)B * B * sizeof(float));
    if (field) {
        veg_bark_height_field(field, B, B);
        set_material_albedo_tex(
            g_mat_bark, load_texture_from_memory_owned(scene->tex_pool, "forest_bark_albedo",
                                                       veg_bark_albedo(B, B, field), B, B, 3, true));
        set_material_normal_tex(
            g_mat_bark, load_texture_from_memory_owned(scene->tex_pool, "forest_bark_normal",
                                                       veg_bark_normal(B, B, field), B, B, 3,
                                                       false));
        set_material_roughness_tex(
            g_mat_bark, load_texture_from_memory_owned(scene->tex_pool, "forest_bark_rough",
                                                       veg_bark_roughness(B, B, field), B, B, 3,
                                                       false));
        free(field);
    }

    const int LW = LEAF_CELL_SIZE * TG_LEAF_VARIANTS;
    const int LH = LEAF_CELL_SIZE;
    unsigned char *la = NULL, *ln = NULL, *lr = NULL;
    veg_leaf_cluster_maps(LW, LH, &la, &ln, &lr);
    set_material_albedo_tex(g_mat_leaf,
                            load_texture_from_memory_owned(scene->tex_pool, "forest_leaf_albedo",
                                                           la, LW, LH, 4, true));
    set_material_normal_tex(g_mat_leaf,
                            load_texture_from_memory_owned(scene->tex_pool, "forest_leaf_normal",
                                                           ln, LW, LH, 3, false));
    set_material_roughness_tex(g_mat_leaf,
                               load_texture_from_memory_owned(scene->tex_pool, "forest_leaf_rough",
                                                              lr, LW, LH, 3, false));
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

// WaterHeightFn over the terrain. A thin adapter rather than a cast, because
// terrain_height_at takes its params first and the water system passes a context
// pointer -- and because the signature is the contract, so it should be written
// out where a reader can see the two agree.
static float forest_bed_height(void* ctx, float x, float z) {
    return terrain_height_at((const TerrainParams*)ctx, x, z);
}

// 1.96 units between cells at the default sizing, against the visual mesh's 2.60,
// so the field resolves everything the tiles can draw with a little to spare.
// Raising it is a straight quadratic on the bake: 452 ms at this size on eight
// threads, 1839 on one.
//
// RELEASE figures. build.sh defaults to the debug preset, which passes no -O at
// all, and the same bake there is 2.3 s and 14.8 s -- five times slower, because
// none of the sim's small helpers inline. Any timing taken from `out/bin` rather
// than `out/release/bin` is measuring that.
#define EROSION_DEFAULT_RES 512

// Seed a field from the fbm, erode it, and install it. Everything downstream --
// tiles, collider, scatter, water bed, camera -- then reads the eroded surface
// through the same terrain_height_at they already called, which is the whole
// point of the field being a source rather than a subsystem.
static void bake_erosion(void) {
    int res = g_args.erode_res > 0 ? g_args.erode_res : EROSION_DEFAULT_RES;
    if (!terrain_field_alloc(&g_field, res)) {
        fprintf(stderr, "forest: erosion field %dx%d allocation failed\n", res, res);
        return;
    }
    if (!terrain_field_seed(&g_field, &g_terrain)) {
        terrain_field_free(&g_field);
        fprintf(stderr, "forest: erosion field seed refused\n");
        return;
    }

    ErosionParams ep = erosion_default_params();
    if (g_args.erode_iterations > 0)
        ep.iterations = g_args.erode_iterations;
    ep.workers = g_args.erode_workers;

    // Zero iterations installs the SEEDED field and runs no sim. Not a degenerate
    // case to tolerate but the one configuration in which the field and the fbm
    // must agree exactly, which is what makes it worth a flag value: sampling a
    // field at a node has to return the fbm value that was written there, and
    // that is the whole node convention asserted end to end.
    if (g_args.erode_iterations < 0) {
        terrain_field_measure(&g_field);
        g_terrain.field = &g_field;
        printf("Terrain seeded: %dx%d cells at %.2f units, no erosion\n", res, res,
               (double)terrain_field_cell(g_terrain.extent, res));
        return;
    }

    ErosionStats st;
    double t0 = glfwGetTime();
    bool ok = terrain_erode(&g_field, &g_terrain, &ep, &st);
    double ms = (glfwGetTime() - t0) * 1000.0;
    if (!ok) {
        terrain_field_free(&g_field);
        fprintf(stderr, "forest: erosion bake refused\n");
        return;
    }

    // Installed only now. Seeding through a params that already pointed at the
    // field would have sampled the plane it was filling.
    g_terrain.field = &g_field;
    float cell = terrain_field_cell(g_terrain.extent, res);
    printf("Terrain eroded: %dx%d cells at %.2f units, %d iterations, %d threads, %.0f ms\n", res,
           res, cell, ep.iterations, st.workers, ms);

    if (g_args.erode_save) {
        float lo = g_field.min_y, hi = g_field.max_y;
        if (heightmap_save(&g_field, g_args.erode_save, lo, hi) && g_args.erode_probe) {
            // At full precision, and this row is why: the range is not recorded in
            // a headerless file, so reading the terrain back needs a number the
            // log's three decimals cannot carry.
            printf("terrain-erosion-probe saved path=%s min=%.9g max=%.9g\n", g_args.erode_save,
                   (double)lo, (double)hi);
        }
    }

    if (g_args.erode_probe)
        erosion_stats_probe(&st, &ep, res, cell, ms);
}

// Install a heightfield from a file, in place of baking one. This is the AAA path
// and the reason the bake writes what this reads: a Gaea or World Machine export
// arrives through exactly here.
static void load_heightfield(void) {
    float lo = g_args.heightmap_min, hi = g_args.heightmap_max;
    if (!(hi > lo)) {
        // The fbm this replaces produces roughly [-height, +height], so a file
        // with no stated range is read as covering the same span.
        lo = -g_terrain.height;
        hi = g_terrain.height;
    }
    if (!heightmap_load(&g_field, g_args.heightmap, lo, hi)) {
        fprintf(stderr, "forest: heightmap %s refused; keeping the analytic terrain\n",
                g_args.heightmap);
        return;
    }
    g_terrain.field = &g_field;
    printf("Terrain loaded: %s, %dx%d, range %.3f..%.3f\n", g_args.heightmap, g_field.res,
           g_field.res, (double)lo, (double)hi);
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
        // Tight clumps: groves with real clearings between them. Below the
        // gravel band's own low edge, so no tree stands where the splat has
        // started painting a stream bed.
        if (!sample_ground(0.86f, TREE_MAX_FLOW, 0.0022f, 2.2f, p))
            continue;
        float s = TREE_WORLD_SCALE * rnd_range(0.65f, 1.6f);
        glm_vec3_copy(p, items[placed].pos);
        glm_vec3_copy((vec3){s, s, s}, items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)TREE_PROTOTYPES);
        items[placed].key = g_args.no_spatial_sort ? 0u : morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);

    if (g_args.scatter_probe)
        scatter_probe(items, placed);

    // Bark and leaves go into SEPARATE group sets. A node holding both would put
    // a leaf item between every pair of bark items, and the run finder breaks on
    // the first item this pass does not want -- so every run would be length one
    // and batching would silently do nothing.
    emit_placements(items, placed, bark, TREE_PROTOTYPES, "bark");
    emit_placements(items, placed, leaf, TREE_PROTOTYPES, "leaf");
    // Release the creation reference now that every holder has taken its own.
    // A prototype that drew no placements is destroyed here rather than leaked.
    for (int i = 0; i < TREE_PROTOTYPES; ++i) {
        free_mesh(bark[i]);
        free_mesh(leaf[i]);
    }
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
        // mirror the tree groves. No drainage limit: a boulder in a stream bed
        // is where boulders are, and excluding them would leave the one place
        // the terrain is painted gravel conspicuously swept clean.
        if (!sample_ground(0.55f, 1.0f, 0.0035f, 1.3f, p))
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
        // One draw per statement. Inside an initializer list these three are
        // indeterminately sequenced (C11 6.7.9p23), and each one advances the
        // shared generator -- so the compiler would be free to pick the order,
        // reshaping every rock and shifting the whole stream after it.
        float sx = rnd_range(0.85f, 1.25f);
        float sy = rnd_range(0.7f, 1.05f);
        float sz = rnd_range(0.85f, 1.25f);
        glm_vec3_copy((vec3){s * sx, s * sy, s * sz}, items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)ROCK_PROTOTYPES);
        items[placed].key = g_args.no_spatial_sort ? 0u : morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);
    emit_placements(items, placed, rocks, ROCK_PROTOTYPES, "rock");
    for (int i = 0; i < ROCK_PROTOTYPES; ++i)
        free_mesh(rocks[i]);
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
    // Lower than the tree app's 10: that app frames one subject against a
    // backdrop and can afford to blow its highlights, where a whole terrain of
    // mid-albedo surfaces facing the sun clips instead.
    sky->sun_base_intensity = 6.0f;
    sky_apply_sun_to_light(sky);
    add_light_to_scene(g_scene, sun);

    SceneNode* node = create_node();
    set_node_name(node, "sun");
    set_node_light(node, sun);
    add_child_node(g_root, node);
}

// --- callbacks -------------------------------------------------------------

// Everything this app holds in world space that the engine cannot reach
// (spec 11.62). The scene graph, the camera and the lights move themselves; what
// is left is another subsystem's storage and this app's own idea of where the
// ground is.
static void forest_on_origin_shift(const vec3 delta, void* ctx) {
    Game* game = (Game*)ctx;

    // The terrain's HEIGHT FUNCTION, which is the ground truth the collider, the
    // scatter and the camera all query. Its mesh moved with the graph, so leaving
    // this behind puts the surface the player walks on a whole delta from the one
    // that gets drawn -- the two would not even disagree visibly, since neither
    // is rendered.
    g_terrain.center[0] -= delta[0];
    g_terrain.center[1] -= delta[2];

    // Jolt stores world positions and is single precision, so it does not follow
    // a scene shift. The CharacterVirtual is separate from every body and has to
    // be moved on its own.
    PhysicsWorld* physics = game ? game_get_physics_world(game) : NULL;
    if (physics) {
        physics_world_shift_origin(physics, delta);
        CharacterController* cc = g_player ? entity_get_character_controller(g_player) : NULL;
        if (cc) {
            vec3 p;
            character_controller_get_position(cc, p);
            glm_vec3_sub(p, (float*)delta, p);
            character_controller_set_position(cc, p);
        }
    }

    // The entity's OWN copy, which is what the follow camera reads. It is
    // refreshed from the controller each step, but not before this frame draws --
    // and a camera that reads the old value places itself a whole delta away,
    // which the automatic shift then reads as more drift and shifts again. The
    // symptom is an origin that oscillates rather than one that is merely late.
    if (g_player)
        glm_vec3_sub(g_player->position, (float*)delta, g_player->position);
}

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
    // Everything else here places itself relative to the terrain, so this one
    // assignment is what moves the world (spec 11.62). The offset is
    // MATERIALISED -- it lands in every vertex, every collider triangle and
    // every scatter position as an fp32 world coordinate -- which is the point:
    // what this measures is what a large world costs before an origin shift.
    g_terrain.center[0] = g_terrain.center[1] = g_args.world_offset;
    // Taller and broader than the library default: at 55 over a 250-unit
    // wavelength the ground reads as flat from anywhere a person stands, and
    // terrain that reads flat gives LOD and culling nothing to work with.
    g_terrain.height = 95.0f;
    g_terrain.base_freq = 0.0026f;
    g_terrain.octaves = 6;
    g_rng = g_args.seed * 2654435761u + 1u;
    noise_perm_init(&g_clump, g_args.seed ^ 0x5bf03635u);

    // Before anything reads a height. The scatter, the collider and the tiles all
    // have to see the same surface, and they see it through g_terrain.
    // A loaded field wins over a baked one: the file is a statement about what
    // the terrain IS, and re-eroding it would be eroding someone's finished work.
    // Said out loud, because every --erode-* flag also SETS --erode, so a command
    // line asking to bake and save alongside a --heightmap would otherwise get no
    // bake, no file and no explanation.
    if (g_args.heightmap && g_args.erode)
        fprintf(stderr, "forest: --heightmap wins over --erode; the bake and any --erode-save "
                        "are skipped\n");
    if (g_args.heightmap)
        load_heightfield();
    else if (g_args.erode)
        bake_erosion();
    if (g_args.height_probe)
        terrain_height_probe(&g_terrain);

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
    g_mat_leaf->foliage_shadows = 1;
    // Thin leaves transmit; without it a backlit canopy reads as opaque plastic.
    g_mat_leaf->subsurface = 0.55f;
    // Wind. Both prototypes come from tree_gen, which already writes the UV1
    // branch phase and flex weight the vegetation modes read, so this is a
    // material field and not new geometry. Modest against a 125-unit trunk: the
    // response multiplies the scene strength, and a canopy that travels metres
    // reads as a storm rather than as air moving.
    g_mat_bark->wind_response = 0.45f;
    g_mat_bark->wind_mode = 1; // whole-trunk lean plus per-branch sway
    g_mat_leaf->wind_response = 0.7f;
    g_mat_leaf->wind_mode = 2; // that, plus the card flutter
    // Dark wet stone. Rock is the only light NEUTRAL surface in a scene of dark
    // foliage, so anything near a realistic granite albedo reads as white against
    // it however the lighting is balanced -- the surrounding palette sets what
    // this can be, not the material on its own.
    g_mat_rock = make_material("rock", (vec3){0.085f, 0.082f, 0.078f}, 0.88f, 0.0f);

    bake_vegetation_textures(g_scene);
    bake_terrain_layers(g_scene);

    // A breeze across the valley. phase_variation is what makes TREE_COUNT copies
    // of TREE_PROTOTYPES meshes look like trees rather than one tree drawn two
    // thousand times: wind is evaluated in object space, so without it every
    // instance sways on the same beat.
    Wind* wind = create_wind("valley breeze");
    if (wind) {
        glm_vec3_copy((vec3){0.82f, 0.0f, 0.57f}, wind->direction);
        wind->strength = 3.0f;
        wind->speed = 0.9f;
        wind->gust_frequency = 0.18f;
        wind->gust_amount = 0.5f;
        wind->turbulence = 0.35f;
        wind->phase_variation = 1.0f;
        set_scene_wind(g_scene, wind);
    }

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

    // Flood it. This is the whole point of the water system's height provider:
    // terrain_height_at is a pure function of (params, x, z), so it satisfies
    // WaterHeightFn directly and the surface can shoal against real terrain with
    // no heightmap stored anywhere and no data copied.
    if (g_args.water) {
        Water* water = create_water();
        if (water) {
            water->level = g_args.water_level;
            // The SHOALING BED's domain, which is the terrain square, because that is
            // exactly where a bed exists to shoal against. It does not bound the drawn
            // surface, which reaches the horizon regardless (spec 11.35) -- past the
            // terrain the bed field reads its edge and the sea is open water, which is
            // what a flooded island should look like from the shore anyway.
            water->extent = g_terrain.extent;
            water->height_at = forest_bed_height;
            water->height_ctx = &g_terrain;
            g_scene->water = water;
        }
    }

    // Standing on the surface, not dropped onto it. The capsule's origin sits
    // half_height + radius above whatever it rests on, so spawning any higher
    // means the first second of every run is a fall -- which reads as the
    // character sliding before it settles.
    const float capsule_rest = 0.9f + 0.4f;
    float spawn_x = g_terrain.center[0], spawn_z = g_terrain.center[1];
    float spawn_y = terrain_height_at(&g_terrain, spawn_x, spawn_z) + capsule_rest + 0.02f;
    g_player = create_entity(em, "player");
    entity_set_position(g_player, (vec3){spawn_x, spawn_y, spawn_z});
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

    scene_set_origin_callback(g_scene, forest_on_origin_shift, game);
    g_scene->origin_shift_distance = g_args.origin_shift_distance;

    // Shadows sized AND placed to the terrain, not left at the library defaults
    // (ortho 2000 about the origin). The scene-fit map covers ortho_size about
    // its centre, so a terrain that is not at the origin needs both.
    ShadowSystem* ss = g_scene->shadow_system;
    if (ss) {
        ss->enabled = true;
        ss->scene_center[0] = g_terrain.center[0];
        ss->scene_center[2] = g_terrain.center[1];
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
    set_camera_position(camera, (vec3){spawn_x, spawn_y + 6.0f, spawn_z + 16.0f});
    set_camera_look_at(camera, (vec3){spawn_x, spawn_y, spawn_z});
    set_camera_up_vector(camera, (vec3){0.0f, 1.0f, 0.0f});
    set_engine_camera(engine, camera);
    set_engine_camera_mode(engine, CAMERA_MODE_FREE);

    // Pinned rather than adaptive: auto-exposure is the top determinism hazard
    // for anything compared across builds, and every arm here reads a frame or a
    // counter from this app.
    engine->exposure.automatic = false;
    engine->exposure.multiplier = 1.8f;

    if (g_args.render_mode > 0)
        engine->current_render_mode = (RenderMode)g_args.render_mode;

    if (g_args.no_lod)
        engine->lod_enabled = false;
    if (g_args.no_instancing)
        engine->instancing_enabled = false;
    if (g_args.no_sort_opaque)
        engine->opaque_sort_enabled = false;
    if (g_args.depth_prepass)
        engine->depth_prepass_enabled = true;
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

// Snap the new origin to a coarse power-of-two lattice rather than taking the
// camera exactly. An arbitrary camera position makes the delta an arbitrary
// float, so every position in the world takes a fresh rounding on every shift;
// a lattice point is exactly representable and, where it matters most -- within
// a factor of two of the coordinates being moved -- the subtraction is exact.
#define ORIGIN_SHIFT_LATTICE 256.0f

static void on_update(Game* game, double dt) {
    // Diagnostic (--origin-shift-at), before the player guard so it runs on a
    // scene with no character: this is about the world, not about who is in it.
    if (g_args.origin_shift_at > 0 && g_scene && game->engine && game->engine->camera &&
        game->engine->total_frames == (size_t)g_args.origin_shift_at) {
        vec3 origin;
        for (int i = 0; i < 3; ++i)
            origin[i] = floorf(game->engine->camera->position[i] / ORIGIN_SHIFT_LATTICE + 0.5f) *
                        ORIGIN_SHIFT_LATTICE;
        scene_set_world_origin(g_scene, origin);
    }

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
            // Re-pinned every frame, so it has to be expressed in the frame the
            // world is CURRENTLY stored in: the flag is a world coordinate
            // somebody typed, which is authoring space, and after an origin shift
            // that is no longer the same as storage. Writing the absolute back
            // each frame would silently undo the shift for the camera alone and
            // leave it looking at where the world used to be.
            vec3 eye, target;
            glm_vec3_sub(g_args.cam_eye, g_scene->world_origin, eye);
            glm_vec3_sub(g_args.cam_target, g_scene->world_origin, target);
            set_camera_position(camera, eye);
            set_camera_look_at(camera, target);
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
            vec3 eye = {target[0] - sinf(g_cam_yaw) * ch * CAM_DISTANCE,
                        target[1] + sinf(g_cam_pitch) * CAM_DISTANCE,
                        target[2] - cosf(g_cam_yaw) * ch * CAM_DISTANCE};
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
    // g_terrain borrows this, so it outlives every consumer by construction and
    // is released only once nothing can ask for a height again.
    g_terrain.field = NULL;
    terrain_field_free(&g_field);
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
    fprintf(stderr, "      --no-sort-opaque    Draw opaques in graph order\n");
    fprintf(stderr, "      --depth-prepass     Depth-only pass before shading\n");
    fprintf(stderr, "      --taa               TAA headless too (diagnostic)\n");
    fprintf(stderr, "      --msaa <n>          Sample count, overriding the TAA/headless policy\n");
    fprintf(stderr, "      --headless-jitter   Sub-pixel jitter headless\n");
    fprintf(stderr, "      --no-sky            Plain directional rig, no atmosphere\n");
    fprintf(stderr, "      --no-fog            Disable the volumetric fog\n");
    fprintf(stderr, "      --no-aerial         Keep the sky, drop aerial perspective\n");
    fprintf(stderr, "      --sun-elevation <d> Sun elevation in degrees\n");
    fprintf(stderr, "      --sun-azimuth <d>   Sun azimuth in degrees\n");
    fprintf(stderr, "      --no-spatial-sort   Scatter without Morton ordering\n");
    fprintf(stderr, "      --trace-player      Log position, velocity and ground state\n");
    fprintf(stderr, "      --render-mode N     1 = normals, 6 = albedo\n");
    fprintf(stderr, "      --lod-bias F        >1 holds detail longer\n");
    fprintf(stderr, "      --water             Flood the terrain (spec 11.32)\n");
    fprintf(stderr, "      --water-level <f>   Still-water world Y (implies --water)\n");
    fprintf(stderr, "      --erode             Bake a heightfield and erode it\n");
    fprintf(stderr, "      --erode-res <n>     Field resolution (default 512)\n");
    fprintf(stderr, "      --erode-iterations <n>  Sim steps (default 220)\n");
    fprintf(stderr, "      --erode-workers <n> Pin the thread count (0 = machine)\n");
    fprintf(stderr, "      --terrain-erosion-probe  Print the bake's own numbers\n");
    fprintf(stderr, "      --erode-save <p>    Write the baked field as .r16\n");
    fprintf(stderr, "      --heightmap <p>     Load a field (.r16 / 16-bit PNG) instead\n");
    fprintf(stderr, "      --heightmap-range <lo> <hi>  World Y the file's range maps to\n");
    fprintf(stderr, "      --terrain-height-probe   Print sampled heights, normals and masks\n");
    fprintf(stderr, "      --scatter-probe          Print the drainage the scatter placed into\n");
    fprintf(stderr, "      --seed N            Terrain and scatter seed\n");
    fprintf(stderr, "      --world-offset N    Place the whole world N units from the origin\n");
    fprintf(stderr, "      --origin-shift-at F Re-centre the world on the camera at frame F\n");
    fprintf(stderr, "      --origin-shift-distance D  Re-centre whenever the camera drifts D\n");
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
        } else if (!strcmp(a, "--no-sort-opaque")) {
            g_args.no_sort_opaque = 1;
        } else if (!strcmp(a, "--depth-prepass")) {
            g_args.depth_prepass = 1;
        } else if (!strcmp(a, "--taa")) {
            g_args.force_taa = 1;
        } else if (!strcmp(a, "--msaa") && i + 1 < argc) {
            g_args.msaa = atoi(argv[++i]);
        } else if (!strcmp(a, "--headless-jitter")) {
            g_args.headless_jitter = 1;
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
        } else if (!strcmp(a, "--water")) {
            g_args.water = 1;
        } else if (!strcmp(a, "--water-level") && i + 1 < argc) {
            g_args.water_level = strtof(argv[++i], NULL);
            g_args.water = 1;
        } else if (!strcmp(a, "--erode")) {
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-res") && i + 1 < argc) {
            g_args.erode_res = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-iterations") && i + 1 < argc) {
            g_args.erode_iterations = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-workers") && i + 1 < argc) {
            g_args.erode_workers = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--terrain-erosion-probe")) {
            g_args.erode_probe = 1;
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-save") && i + 1 < argc) {
            g_args.erode_save = argv[++i];
            g_args.erode = 1;
        } else if (!strcmp(a, "--heightmap") && i + 1 < argc) {
            g_args.heightmap = argv[++i];
        } else if (!strcmp(a, "--heightmap-range") && i + 2 < argc) {
            g_args.heightmap_min = strtof(argv[++i], NULL);
            g_args.heightmap_max = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--scatter-probe")) {
            g_args.scatter_probe = 1;
        } else if (!strcmp(a, "--terrain-height-probe")) {
            g_args.height_probe = 1;
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
        } else if (!strcmp(a, "--world-offset") && i + 1 < argc) {
            g_args.world_offset = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--origin-shift-at") && i + 1 < argc) {
            g_args.origin_shift_at = atoi(argv[++i]);
        } else if (!strcmp(a, "--origin-shift-distance") && i + 1 < argc) {
            g_args.origin_shift_distance = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--cam-eye") && i + 1 < argc) {
            cam_eye_set = parse_vec3(argv[++i], g_args.cam_eye);
        } else if (!strcmp(a, "--cam-target") && i + 1 < argc) {
            cam_target_set = parse_vec3(argv[++i], g_args.cam_target);
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
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

    game->engine->headless_jitter = g_args.headless_jitter != 0;
    // TAA replaces MSAA rather than joining it, which is what the render app has
    // always done and what this app was missing: it shipped 4x MSAA with no
    // temporal filter at all, so masked foliage got raw coverage dither and the
    // dark A2C fringe spec 11.19 measured, with nothing to resolve either.
    //
    // Headless keeps MSAA and skips TAA unless asked, because jitter plus a
    // history makes the frame sensitive to async load timing -- the same
    // diagnostic-only escape the render app carries.
    if (!g_args.headless || g_args.force_taa) {
        set_engine_msaa_samples(game->engine, 1);
        set_engine_taa_enabled(game->engine, true);
    }
    // After the policy, so --taa --msaa 4 is expressible -- the lever that
    // prices a sample (spec 11.34); nothing else varies the count with TAA held.
    if (g_args.msaa > 0)
        set_engine_msaa_samples(game->engine, g_args.msaa);

    run_game(game);
    free_game(game);
    return 0;
}
