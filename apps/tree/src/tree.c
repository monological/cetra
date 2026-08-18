#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/shader.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/render.h"
#include "cetra/geometry.h"
#include "cetra/transform.h"
#include "cetra/light.h"
#include "cetra/texture.h"
#include "cetra/app.h"
#include "cetra/sky.h"
#include "cetra/water.h"
#include "cetra/ibl.h"
#include "cetra/shadow.h"
#include "cetra/wind.h"
#include "cetra/postfx.h"
#include "cetra/particle_system.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"

#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/rock.h"
#include "cetra/procedural/sand.h"
#include "cetra/procedural/stochastic_tex.h"
#include "cetra/procedural/vegetation_tex.h"
#include "ground.h"
#include "player.h"
#include "grass.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#define TEXTURE_SIZE      512
#define BARK_TEXTURE_SIZE 1024

/*
 * Generate all procedural textures
 */
static Texture* bark_albedo_tex = NULL;
static Texture* bark_normal_tex = NULL;
static Texture* bark_roughness_tex = NULL;
static Texture* bark_height_tex = NULL;
static Texture* leaf_albedo_tex = NULL;
static Texture* leaf_normal_tex = NULL;
static Texture* leaf_roughness_tex = NULL;
static Texture* leaf_sprite_tex = NULL;
static Texture* island_albedo_tex = NULL;
static Texture* island_normal_tex = NULL;
static Texture* island_roughness_tex = NULL;
// The inverse histogram of the sand albedo, filled at bake and handed to the island material.
// Kept here rather than on the texture because it describes the TRANSFORM, and only a material
// that opts into stochastic sampling has any use for it.
static float sand_stochastic_lut[STOCHASTIC_LUT_SIZE * 3];
static bool sand_stochastic_ready = false;

// Bake one CPU buffer into a pooled texture and release the buffer. Going
// through the pool (rather than a hand-rolled glTexImage2D) is what gets the
// albedo maps decoded as sRGB and, for the leaf cutout, gets the transparent
// texels' RGB dilated so mipping doesn't fringe the leaf edges with black.
static Texture* bake_texture(Scene* scene, unsigned char* data, int width, int height,
                             int channels, bool is_srgb, const char* key) {
    if (!data)
        return NULL;
    Texture* tex =
        load_texture_from_memory(scene->tex_pool, key, data, width, height, channels, is_srgb);
    free(data);
    return tex;
}

static void generate_procedural_textures(Scene* scene) {
    const int B = BARK_TEXTURE_SIZE;
    const int T = TEXTURE_SIZE;
    // The leaf atlas is one row of square cluster cells.
    const int LW = TEXTURE_SIZE * TG_LEAF_VARIANTS;
    const int LH = TEXTURE_SIZE;

    printf("Generating procedural bark textures...\n");
    // One relief, four maps derived from it -- they describe the same surface,
    // and the field is only built once instead of per map.
    float* bark_field = malloc((size_t)B * B * sizeof(float));
    if (bark_field) {
        veg_bark_height_field(bark_field, B, B);
        bark_albedo_tex =
            bake_texture(scene, veg_bark_albedo(B, B, bark_field), B, B, 3, true,
                         "proc_bark_albedo");
        bark_normal_tex =
            bake_texture(scene, veg_bark_normal(B, B, bark_field), B, B, 3, false,
                         "proc_bark_normal");
        bark_roughness_tex =
            bake_texture(scene, veg_bark_roughness(B, B, bark_field), B, B, 3, false,
                         "proc_bark_roughness");
        bark_height_tex =
            bake_texture(scene, veg_bark_height(B, B, bark_field), B, B, 1, false,
                         "proc_bark_height");
        free(bark_field);
    }

    printf("Generating procedural leaf cluster atlas...\n");
    unsigned char *leaf_a = NULL, *leaf_n = NULL, *leaf_r = NULL;
    veg_leaf_cluster_maps(LW, LH, &leaf_a, &leaf_n, &leaf_r);
    leaf_albedo_tex = bake_texture(scene, leaf_a, LW, LH, 4, true, "proc_leaf_albedo");
    leaf_normal_tex = bake_texture(scene, leaf_n, LW, LH, 3, false, "proc_leaf_normal");
    leaf_roughness_tex = bake_texture(scene, leaf_r, LW, LH, 3, false, "proc_leaf_roughness");
    leaf_sprite_tex =
        bake_texture(scene, veg_leaf_sprite(T), T, T, 4, true, "proc_leaf_sprite");

    /*
     * The ground is SAND (spec 11.44), and its colour is not in this texture.
     *
     * These three carry grain, ripples and relief only, near-neutral, because vertex colour
     * supplies the hue -- submerged sand, the wet strip at the waterline, dry beach, and the
     * upland at the crown, all from ground_beach_color. That split is forced by the sampler
     * ledger: pbr_frag declares sixteen of sixteen, so the terrain gets ONE albedo map and a
     * second one for the upland cannot be bound. It also buys a continuous grade instead of
     * a material boundary between beach and inland.
     *
     * The ripple train is oriented across the radial direction, which on a round island is
     * along the shore -- which is the way a beach's ripples actually run.
     */
    printf("Generating procedural sand textures...\n");
    float* sand_field = malloc((size_t)T * T * sizeof(float));
    if (sand_field) {
        veg_noise_seed(4242);
        sand_height_field(sand_field, T, T, 0.7853982f); // 45 degrees across the UV diagonal
        /*
         * The albedo goes through the histogram transform before it is uploaded, so the
         * shader can sample it stochastically and stop the tile being recognisable. See
         * procedural/stochastic_tex.h; the material picks the table up below.
         *
         * Baked NON-sRGB, which is the one thing easy to get wrong here: the transform and
         * its inverse are both defined on the stored codes, so the decode has to happen after
         * the inverse table rather than in the sampler. pbr_frag does it there.
         */
        unsigned char* sand_alb = sand_albedo(T, T, sand_field);
        stochastic_gaussianize(sand_alb, T, T, sand_stochastic_lut);
        sand_stochastic_ready = sand_alb != NULL;
        island_albedo_tex = bake_texture(scene, sand_alb, T, T, 3, false, "proc_sand_albedo");
        island_normal_tex =
            bake_texture(scene, sand_normal(T, T, sand_field), T, T, 3, false, "proc_sand_normal");
        island_roughness_tex = bake_texture(scene, sand_roughness(T, T, sand_field), T, T, 3,
                                            false, "proc_sand_roughness");
        free(sand_field);
    }

    printf("Procedural textures generated.\n");

    // Clear any pending GL errors and reset state to avoid affecting subsequent operations
    while (glGetError() != GL_NO_ERROR) {
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

/*
 * Constants
 */
const unsigned int HEIGHT = 900;
const unsigned int WIDTH = 1400;

/*
 * Globals
 */
static TreeParams params;
static TreeParams prev_params;
static Material* bark_material = NULL;
static Material* leaf_material = NULL;
static SceneNode* tree_root = NULL;
static SceneNode* island_node = NULL;
static Material* island_material = NULL;
static SceneNode* seabed_node = NULL;
static Material* seabed_material = NULL;
static Material* rock_material = NULL;

static GrassParams grass_params;
static GrassParams prev_grass_params;
static SceneNode* grass_node = NULL;
static Material* grass_material = NULL;

static SkyAtmosphere* sky = NULL;
static IBLResources* ibl = NULL;
static Light* sun_light = NULL;
static Wind* scene_wind = NULL;

// Falling leaves: the spawn module is held so the GUI can gate it without
// tearing down the emitter (existing leaves finish their fall).
static ParticleModule* leaf_spawn_module = NULL;
static bool falling_leaves_on = true;
static float leaf_spawn_rate = 2.5f;

// Season tint multiplies the leaf albedo texture: 0 = summer green, 1 = autumn.
static float season = 0.0f;
static float prev_season = -1.0f;

/*
 * A sunset, and both numbers are derived rather than dialled.
 *
 * ELEVATION 0.8, chosen for the look. Low sun is what the Hillaire atmosphere turns orange --
 * the slant path through the air is long, Rayleigh scattering takes the blue out of it, and
 * because the sky couples the key light through its own transmittance LUT the DIRECT light warms
 * with the sky rather than only the backdrop. Measured on the beach, which is a diffuse
 * near-neutral surface so its tint is the light's tint: R/B goes 1.42 at 14 degrees to 1.59 at 6,
 * then inverts toward blue below about 2 as the sun extinguishes and skylight becomes all there
 * is -- real twilight, not a bug, and this default sits inside that last stretch.
 *
 * Two things it gives up, recorded so they are not rediscovered as defects. The direct sun is
 * nearly gone at this elevation, so the frame is lit mostly by sky and is DARK. And the shadow
 * map cannot hold it: the span it needs is roughly 2 * (radius + treeHeight / tan(elevation)),
 * which is 4,798 units at 8 degrees against a 6,000 budget and about 37,000 here -- so shadows
 * run off the map rather than reaching across the island. --sun-elevation 8 is the framing that
 * keeps both.
 *
 * AZIMUTH 193. sun_dir is (cos(el)sin(az), sin(el), cos(el)cos(az)), so putting the sun behind
 * the tree from a camera at (140, ., 600) is atan2(-140, -600) = 193 degrees. That is what makes
 * it a sunset rather than a low side-light: the glow blooms through the canopy, the sea carries
 * it back, and the trunk goes to silhouette. A few degrees more brings the disc out from behind
 * the tree -- 210 shows it clear of the trunk, over open water.
 */
static float sun_elevation = 0.8f;
static float sun_azimuth = 193.0f;

/*
 * Mouse drag controller
 */
static MouseDragController* drag_controller = NULL;
// Non-NULL only under --player; the orbit controller and the walker both own the camera, so
// exactly one of them exists at a time.
static Player* player = NULL;

/*
 * The SHOALING BED's domain, and nothing else -- since spec 11.35 the grid is projected from
 * the frustum and reaches the horizon at any extent. So this is sized for the SHORE BAND
 * rather than for the island. Outside it the bed field reads its edge, which is open water --
 * correct, because the per-fragment water column comes from the depth buffer and not here.
 *
 * 900, against the 400 it was through 11.43. Two things moved it. The shoal window was being
 * read as world units in a world at 22 units to the metre, so the ramp it was sized against
 * measured 8.4 units instead of its real width; and 11.44 then flattened the beach from a
 * slope of 0.31 to 0.145, which spreads the same window of DEPTH over more than twice the
 * ground. The surf band now runs from radius 366 to 584, and a 400-unit domain cut most of
 * it off with the bed reporting open water there.
 *
 * The texel density it was also guarding still holds: 2*900/WATER_BED_RES is 7.0 units per
 * texel against a ramp now 218 units wide, which is thirty-one texels rather than three.
 */
#define TREE_WATER_EXTENT 900.0f

// WaterHeightFn over the dome. ground_height_at takes no context -- it reads two
// compile-time constants -- so the adapter drops the unused pointer rather than the
// water system special-casing a nullary provider.
static float tree_bed_height(void* ctx, float x, float z) {
    (void)ctx;
    return ground_height_at(x, z);
}

/*
 * Create the ground.
 *
 * Wide enough to reach the horizon: at the old radius of 120 the disc read as a saucer
 * floating in the sky's dark virtual ground rather than as terrain. Built about its crown
 * and translated down by its own height, so the crown lands at y = 0 where the tree roots
 * start -- which is the convention ground_height_at reports in.
 */
static void create_island(SceneNode* parent) {
    island_node = create_node();
    set_node_name(island_node, "ground");

    Mesh* mesh = create_mesh();
    // 128 rings, not the 24 this had: the beach's colour bands are VERTEX colour, and the
    // wet strip is 0.35 m of a 2.16 m rise. At 24 rings a ring is 1.2 m, so the whole wet
    // band fell between two of them and the grade came out as steps. 128 puts a ring every
    // 0.22 m, which resolves the narrowest band.
    //
    // The segment count is DERIVED (ground.h) rather than chosen: it is the shoreline, not
    // the disc, that sets it.
    ground_build_mesh(mesh, 128, GROUND_MESH_SEGMENTS, 40.0f);
    mesh->material = island_material;

    glm_mat4_identity(island_node->original_transform);
    glm_translate(island_node->original_transform, (vec3){0.0f, -GROUND_HEIGHT, 0.0f});

    add_mesh_to_node(island_node, mesh);
    add_child_node(parent, island_node);
    // Static for the program's lifetime, so it uploads once here rather than
    // riding along with every tree rebuild.
    upload_buffers_to_gpu_for_nodes(island_node);
}

/*
 * Create the seabed (spec 11.35 phase 6).
 *
 * Authored in WORLD space, unlike the island, which is built about its crown and translated:
 * the seabed's whole job is to continue the island's flank, and reading ground_height_at
 * directly is how it shares that edge rather than approaching it.
 */
static void create_seabed(SceneNode* parent) {
    Mesh* mesh = create_mesh();
    // The SAME segment count as the island, which is what makes the shared rim row shared:
    // the two meshes meet vertex for vertex there rather than as a 96-gon inscribed in a
    // 64-gon, which is what they were.
    if (!ground_build_seabed(mesh, 40, GROUND_MESH_SEGMENTS, 60.0f)) {
        free_mesh(mesh);
        return;
    }
    mesh->material = seabed_material;

    seabed_node = create_node();
    set_node_name(seabed_node, "seabed");
    add_mesh_to_node(seabed_node, mesh);
    add_child_node(parent, seabed_node);
    upload_buffers_to_gpu_for_nodes(seabed_node);
}

/*
 * Boulders through the surf (spec 11.44).
 *
 * Placed by DEPTH rather than by radius, straddling the waterline: a rock is only worth
 * having where it interrupts something, and what it interrupts here is the shore band. So
 * the placement reads the same ground_height_at the water shoals against, which also means
 * they follow the profile if it ever changes rather than sitting at a transcribed radius.
 *
 * Each rock is its own mesh and its own node. At this count that is cheaper to read than a
 * prototype table, and nothing here is instanced: apps/forest exists to exercise that path
 * with thousands, where this wants a dozen.
 *
 * Sunk by a third of the radius so they sit IN the sand rather than on it. A boulder
 * tangent to the ground reads as dropped, and the shoreline is exactly where the eye checks.
 */
#define TREE_ROCK_COUNT 14
#define TREE_ROCK_MIN_M 0.30f
#define TREE_ROCK_MAX_M 1.10f
// Depths the scatter spans, in metres of water: above the line is a dry boulder on the
// beach, below it one standing in the shallows.
#define TREE_ROCK_HIGH_M (-0.45f)
#define TREE_ROCK_LOW_M 1.30f

static SceneNode* rock_nodes[TREE_ROCK_COUNT];

static void create_shore_rocks(SceneNode* parent) {
    if (!rock_material)
        return;
    veg_noise_seed(9317); // the scatter's own stream, seeded next to its only consumer

    const float shore = ground_shore_height();
    for (int i = 0; i < TREE_ROCK_COUNT; i++) {
        RockParams rp = rock_default_params();
        rp.seed = 41u + (unsigned)i * 977u;
        rp.subdivisions = 3;
        rp.roughness = 0.24f + veg_rand_range(0.0f, 0.10f);
        rp.noise_freq = 1.3f + veg_rand_range(0.0f, 0.7f);
        rp.radius = veg_rand_range(TREE_ROCK_MIN_M, TREE_ROCK_MAX_M) * GROUND_UNITS_PER_METRE;

        Mesh* mesh = create_mesh();
        if (!rock_build_mesh(&rp, mesh)) {
            free_mesh(mesh);
            continue;
        }
        mesh->material = rock_material;

        /*
         * Solve for the radius that puts this rock at its chosen height above the water.
         * The profile is monotonic outward over the beach, so a bisection is exact and needs
         * no inverse -- the same reasoning that let the water level stop being closed-form.
         */
        const float want = shore - veg_rand_range(TREE_ROCK_HIGH_M, TREE_ROCK_LOW_M) *
                                       GROUND_UNITS_PER_METRE;
        float lo = 0.0f, hi = GROUND_RADIUS;
        for (int it = 0; it < 40; it++) {
            const float mid = 0.5f * (lo + hi);
            if (ground_height_at(mid, 0.0f) > want)
                lo = mid;
            else
                hi = mid;
        }
        const float r = 0.5f * (lo + hi);
        const float angle = veg_rand_range(0.0f, 6.28318531f);
        const float x = r * cosf(angle);
        const float z = r * sinf(angle);

        SceneNode* node = create_node();
        set_node_name(node, "shore_rock");
        add_mesh_to_node(node, mesh);
        glm_mat4_identity(node->original_transform);
        glm_translate(node->original_transform,
                      (vec3){x, ground_height_at(x, z) - rp.radius * 0.34f, z});
        glm_rotate(node->original_transform, veg_rand_range(0.0f, 6.28318531f),
                   (vec3){0.0f, 1.0f, 0.0f});
        // Squashed a little, because a displaced icosphere is round and a boulder that has
        // been sitting in surf is not.
        glm_scale(node->original_transform, (vec3){1.0f, veg_rand_range(0.62f, 0.88f), 1.0f});
        add_child_node(parent, node);
        upload_buffers_to_gpu_for_nodes(node);
        rock_nodes[i] = node;
    }
}

/*
 * Regenerate tree
 *
 * All the bark lands in one mesh and all the leaves in another, so the whole
 * tree is two draw calls no matter how many branches the sliders ask for.
 *
 * The node itself outlives every rebuild and only its meshes are swapped. An
 * earlier version rebuilt by walking the root and freeing any child that was
 * not a light or the ground -- a blacklist, which silently freed the
 * falling-leaves node as soon as that was added, while the Scene still held
 * its particle system and dereferenced the dead node every tick.
 */
static void regenerate_tree(const TreeParams* p) {
    if (!tree_root)
        return;

    for (size_t i = 0; i < tree_root->mesh_count; i++)
        free_mesh(tree_root->meshes[i]);
    tree_root->mesh_count = 0;

    TreeSkeleton skel;
    memset(&skel, 0, sizeof(skel));
    tree_skeleton_build(&skel, p);

    Mesh* bark = create_mesh();
    if (tree_mesh_bark(&skel, p, bark)) {
        bark->material = bark_material;
        add_mesh_to_node(tree_root, bark);
    } else {
        free_mesh(bark);
    }

    Mesh* leaves = create_mesh();
    if (tree_mesh_leaves(&skel, p, leaves)) {
        leaves->material = leaf_material;
        add_mesh_to_node(tree_root, leaves);
    } else {
        free_mesh(leaves);
    }

    printf("Tree: %d branches, %zu bark verts, %zu leaf verts\n", skel.branch_count,
           tree_root->mesh_count > 0 ? tree_root->meshes[0]->vertex_count : (size_t)0,
           tree_root->mesh_count > 1 ? tree_root->meshes[1]->vertex_count : (size_t)0);

    tree_skeleton_free(&skel);

    // Only the tree's own meshes: the ground is static and uploaded once.
    upload_buffers_to_gpu_for_nodes(tree_root);
}

/*
 * Regenerate the grass field
 *
 * Same shape as the tree: the node outlives every rebuild and only its mesh is
 * swapped, so nothing else parented to the root is ever at risk.
 */
static void regenerate_grass(const GrassParams* p) {
    if (!grass_node)
        return;

    for (size_t i = 0; i < grass_node->mesh_count; i++)
        free_mesh(grass_node->meshes[i]);
    grass_node->mesh_count = 0;

    Mesh* grass = create_mesh();
    if (grass_build_mesh(p, grass)) {
        grass->material = grass_material;
        add_mesh_to_node(grass_node, grass);
        printf("Grass: %zu verts\n", grass->vertex_count);
    } else {
        free_mesh(grass);
    }

    upload_buffers_to_gpu_for_nodes(grass_node);
}

// Leaf color across the season slider. The albedo factor multiplies the leaf
// texture, so this rides on top of the procedural green rather than replacing
// it; the subsurface tint follows so backlit leaves warm up with the canopy.
static void apply_season(float t) {
    if (!leaf_material)
        return;
    vec3 summer = {1.0f, 1.0f, 1.0f};
    vec3 autumn = {1.35f, 0.62f, 0.18f};
    glm_vec3_lerp(summer, autumn, t, leaf_material->albedo);

    vec3 green_sss = {0.5f, 0.8f, 0.15f};
    vec3 amber_sss = {0.95f, 0.5f, 0.1f};
    glm_vec3_lerp(green_sss, amber_sss, t, leaf_material->subsurface_color);
}

/*
 * Render tree parameters GUI
 */
static void render_tree_gui(const Engine* engine, Scene* scene) {
    (void)scene;

    if (!engine || !engine->show_gui)
        return;

    igSetNextWindowPos((ImVec2){15, 15}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){300, 720}, ImGuiCond_FirstUseEver);
    if (igBegin("Tree", NULL, 0)) {
        igSeparatorText("Seed");
        igSliderInt("Seed", &params.seed, 0, 9999, "%d", 0);

        igSeparatorText("Structure");
        igSliderInt("Max Depth", &params.max_depth, 1, 6, "%d", 0);
        igSliderInt("Branches", &params.branches_per_node, 1, 5, "%d", 0);
        igSliderFloat("Laterals", &params.lateral_density, 0.0f, 3.0f, "%.2f", 0);

        igSeparatorText("Dimensions");
        igSliderFloat("Trunk Len", &params.trunk_length, 10.0f, 200.0f, "%.1f", 0);
        igSliderFloat("Trunk Rad", &params.trunk_radius, 1.0f, 30.0f, "%.1f", 0);
        igSliderFloat("Len Decay", &params.length_decay, 0.3f, 0.95f, "%.3f", 0);
        igSliderFloat("Taper", &params.taper, 0.45f, 0.85f, "%.3f", 0);
        igSliderFloat("Twig Scale", &params.twig_scale, 0.5f, 2.0f, "%.2f", 0);

        igSeparatorText("Angles");
        igSliderFloat("Angle", &params.branch_angle, 5.0f, 90.0f, "%.1f", 0);
        igSliderFloat("Variance", &params.angle_variance, 0.0f, 45.0f, "%.1f", 0);
        igSliderFloat("Twist", &params.twist, 0.0f, 180.0f, "%.1f", 0);

        igSeparatorText("Curvature");
        igSliderFloat("Droop", &params.droop, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Curve Noise", &params.curve_noise, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Phototropism", &params.phototropism, 0.0f, 1.0f, "%.2f", 0);

        igSeparatorText("Leaves");
        bool show_leaves = params.show_leaves != 0;
        if (igCheckbox("Show Leaves", &show_leaves))
            params.show_leaves = show_leaves;
        igSliderFloat("Leaf Size", &params.leaf_size, 1.0f, 30.0f, "%.1f", 0);
        igSliderFloat("Leaf Density", &params.leaf_density, 0.5f, 8.0f, "%.2f", 0);
        igSliderFloat("Season", &season, 0.0f, 1.0f, "%.2f", 0);

        igSeparatorText("Grass");
        igSliderFloat("Density", &grass_params.density, 0.0f, 12.0f, "%.2f", 0);
        igSliderFloat("Patchiness", &grass_params.patchiness, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Blade Height", &grass_params.height, 1.0f, 14.0f, "%.2f", 0);
        igSliderFloat("Bend", &grass_params.bend, 0.0f, 1.2f, "%.2f", 0);
        igSliderFloat("Flowers", &grass_params.flower_amount, 0.0f, 0.3f, "%.3f", 0);
        igSliderFloat("Seed Heads", &grass_params.seed_head_amount, 0.0f, 0.4f, "%.3f", 0);
        igSliderFloat("Field Radius", &grass_params.radius, 20.0f, 220.0f, "%.0f", 0);

        igSeparatorText("Wind");
        if (scene_wind) {
            igSliderFloat("Strength", &scene_wind->strength, 0.0f, 8.0f, "%.2f", 0);
            igSliderFloat("Speed", &scene_wind->speed, 0.0f, 4.0f, "%.2f", 0);
            igSliderFloat("Gust Freq", &scene_wind->gust_frequency, 0.0f, 2.0f, "%.2f", 0);
            igSliderFloat("Gust Amount", &scene_wind->gust_amount, 0.0f, 1.0f, "%.2f", 0);
            igSliderFloat("Turbulence", &scene_wind->turbulence, 0.0f, 1.0f, "%.2f", 0);
        }

        igSeparatorText("Falling Leaves");
        if (igCheckbox("Enabled", &falling_leaves_on) && leaf_spawn_module) {
            particle_module_spawn_rate_set(leaf_spawn_module,
                                           falling_leaves_on ? leaf_spawn_rate : 0.0f);
        }
        if (igSliderFloat("Rate", &leaf_spawn_rate, 0.0f, 15.0f, "%.1f", 0) && leaf_spawn_module &&
            falling_leaves_on) {
            particle_module_spawn_rate_set(leaf_spawn_module, leaf_spawn_rate);
        }

        igSeparatorText("Atmosphere");
        if (engine->postfx) {
            igCheckbox("Fog", &engine->postfx->fog_enabled);
            igSliderFloat("Fog Density", &engine->postfx->fog_density, 0.0f, 0.0015f, "%.5f", 0);
            igSliderFloat("Fog Height", &engine->postfx->fog_height_falloff, 5.0f, 200.0f, "%.0f",
                          0);
        }

        /*
         * Sun and sky, grouped by WHAT AN EDIT COSTS -- which is not visible from the parameter
         * names, and which two of these were initially filed under wrongly:
         *
         *   RE-BAKE, via sky_update_sun: only what the sky-view LUT and the environment cubemap
         *   are a function of, which is the sun's POSITION and nothing else here. Its header
         *   says it is cheap enough to run per slider frame, and it is the single entry point
         *   for "the sun moved" -- it re-derives sun_dir, re-bakes, and retints the key light.
         *
         *   RETINT, via sky_apply_sun_to_light: the key light's strength. No bake.
         *
         *   FREE: everything else below is a per-frame uniform, read by the pass that consumes
         *   it. Checked rather than assumed -- see the notes on Disc Size and Units per km,
         *   both of which look like sky state and are not.
         */
        igSeparatorText("Sun");
        bool sun_moved = igSliderFloat("Elevation", &sun_elevation, -5.0f, 89.0f, "%.1f deg", 0);
        sun_moved |= igSliderFloat("Azimuth", &sun_azimuth, 0.0f, 360.0f, "%.1f deg", 0);
        if (sun_moved && sky) {
            sky->sun_elevation_deg = sun_elevation;
            sky->sun_azimuth_deg = sun_azimuth;
            sky_update_sun(sky, ibl, (Engine*)engine);
        }
        if (sky) {
            /*
             * Angular DIAMETER of the drawn disc; the real sun is 0.53 degrees.
             *
             * SKYBOX ONLY, and worth knowing before reaching for it: sunCosRadius reaches
             * sky_background_frag and sky_background_clouds_frag and nothing else -- not
             * sky_env_frag. So the disc is absent from the environment cubemap, which means
             * reflections and the water's sun glint do NOT follow it. Free to drag for the same
             * reason: no bake consumes it.
             */
            igSliderFloat("Disc Size", &sky->sun_disc_deg, 0.05f, 6.0f, "%.2f deg", 0);
            // Retint only. The light's COLOUR is deliberately not offered: it comes from
            // atmospheric transmittance, which is what keeps the sun matching the sky it is in,
            // and a picker here would make this a second owner of it.
            if (igSliderFloat("Intensity", &sky->sun_base_intensity, 0.0f, 40.0f, "%.1f", 0))
                sky_apply_sun_to_light(sky);
        }
        // Emitter size, straight onto the light: it drives the PCSS penumbra, so this is the
        // shadow-softness control. Square, because a directional sun has no reason to be
        // oblong and two sliders for one physical quantity is worse than one.
        if (sun_light) {
            igSliderFloat("Shadow Softness", &sun_light->size[0], 0.5f, 40.0f, "%.1f", 0);
            sun_light->size[1] = sun_light->size[0];
        }

        igSeparatorText("Sky");
        if (sky) {
            /*
             * World units per kilometre, and it is the strongest knob in this panel.
             *
             * The atmosphere is modelled in km while a scene is in whatever it was authored in,
             * so this mapping decides how much AIR the frame looks through: lower it and the
             * island sits under kilometres of haze, raise it and the same geometry becomes a
             * tabletop under a clear sky. Logarithmic, because the useful range spans decades.
             *
             * Also free, which was a surprise: it reaches only the per-frame aerial volume and
             * the cloud march, never the baked LUTs -- so the aerial perspective moves with it
             * while the sky-view LUT does not.
             */
            igSliderFloat("Units per km", &sky->world_units_per_km, 1.0f, 100000.0f, "%.0f",
                          ImGuiSliderFlags_Logarithmic);
            igCheckbox("Aerial Perspective", &sky->aerial_enabled);
            igCheckbox("Sky drives Fog Ambient", &sky->publish_fog_ambient);
            igCheckbox("Debug: LUTs", &sky->debug_luts);
        }
        if (scene) {
            igSliderFloat("Skybox Brightness", &scene->skybox_brightness, 0.0f, 4.0f, "%.2f", 0);
            // The radius comes WITH the toggle, not after it: the library default is 5 units,
            // which against a 620-unit island projects the sky onto a puddle at the origin and
            // reads as a bug rather than as a feature that needs sizing.
            igCheckbox("Ground Projection", &scene->skybox_ground_projection);
            if (scene->skybox_ground_projection)
                igSliderFloat("GP Radius", &scene->skybox_gp_radius, 5.0f, 2000.0f, "%.0f",
                              ImGuiSliderFlags_Logarithmic);
        }
    }
    igEnd();
}

/*
 * Callbacks
 */
void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (drag_controller) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(drag_controller, button, action, mods, x, y);
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;

    // Nothing for the walker here: every key it reads is a HELD state, and a key-repeat stream
    // of events is not a velocity. It polls in player_update instead.

    // Camera movement
    if (drag_controller && camera_controller_on_key(drag_controller, key, action, mods)) {
        return;
    }

    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
            break;
        case GLFW_KEY_G:
            set_engine_show_gui(engine, !engine->show_gui);
            break;
        case GLFW_KEY_X:
            set_engine_show_xyz(engine, !engine->show_xyz);
            break;
        case GLFW_KEY_T:
            set_engine_show_wireframe(engine, !engine->show_wireframe);
            break;
        default:
            break;
    }
}

void render_scene_callback(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->root_node) {
        return;
    }

    // Render custom GUI first
    render_tree_gui(engine, scene);

    // Check for parameter changes
    if (memcmp(&params, &prev_params, sizeof(TreeParams)) != 0) {
        regenerate_tree(&params);
        memcpy(&prev_params, &params, sizeof(TreeParams));
    }

    if (season != prev_season) {
        apply_season(season);
        prev_season = season;
    }

    if (memcmp(&grass_params, &prev_grass_params, sizeof(GrassParams)) != 0) {
        regenerate_grass(&grass_params);
        memcpy(&prev_grass_params, &grass_params, sizeof(GrassParams));
    }

    if (player) {
        // The frame delta, not the wall clock: this integrates gravity and a walk speed, and
        // both are per-unit-of-time quantities that have to advance with the frame the surface
        // is about to be drawn for.
        player_update(player, engine, (float)engine->render_delta);
    } else if (drag_controller && app_can_process_3d_input(engine)) {
        // Update camera - only if not hovering over GUI. Deliberately the wall
        // clock, not the frame clock: drag damping is input response.
        mouse_drag_update(drag_controller, glfwGetTime());
    }

    // Apply transforms
    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(scene->root_node, engine->model_matrix);

    render_current_scene(engine);
}

/*
 * Command line
 */
typedef struct {
    int headless;
    int frames;
    int screenshot_every;
    const char* screenshot;
    int width, height;
    int no_shadows;
    int no_fog;
    int no_falling_leaves;
    int seed;
    float sun_elevation;
    float sun_azimuth;
    int no_water;      // the sea is ON here: this app's ground IS an island
    float water_level; // world Y of the still surface (-9999 = the default)
    // Spectral by default here. Gerstner is the A/B, and the only way to reach the
    // wavelength/amplitude/steepness/spread this app authors, which the spectral path ignores.
    int gerstner_waves;
    int no_water_wetness;
    int no_water_film;
    int water_foam_debug; // WaterFoamDebug (water.h): 0 off, 1 eroded, 2 pre-erosion
    int no_water_foam_history; // Bisect lever: foam from this frame's fold only
    // Bisect lever: no incident wave at the shore. Removes the bore from the GEOMETRY as
    // well as the whitewater, since depth-limited breaking is gated on the surf existing.
    int no_water_surf;
    // Spectral sea state: these two address the WIND SEA, --swell below scales the swell
    // train. <=0 keeps this app's own value. The FFT path takes its wave heights from the
    // trains and ignores the wavelength/amplitude below, so these are the only way to make
    // this sea calmer without switching wave models.
    float wind_speed;
    float fetch;
    float swell; // the swell train's `scale`; <0 keeps the default, 0 = no swell train
    // RenderMode override, 0 = PBR. The GUI has always had the combo; without this the debug
    // modes could not be reached headlessly, so anything they diagnose could not be captured.
    int render_mode;
    // MSAA sample count, 0 = leave the engine default (4). This app runs MSAA *and* TAA, which
    // is why the extrapolation specks surfaced here first; without the flag the one comparison
    // that identifies them -- against a single sample, where no coverage is partial -- cannot
    // be made in the app that shows them.
    int msaa;
    /*
     * Keep the TAA jitter under --headless, which is also what lets the render scale stay at
     * the 0.70 the app runs at: TAAU reconstructs from the jitter, so the engine pins the
     * scale to 1 without it.
     *
     * Non-deterministic by construction, so it is not for goldens -- it is for reproducing
     * what the WINDOW draws. Without it a headless capture of this app runs a different
     * pipeline from the app: full resolution, no jitter, no upscaling resolve. An artifact
     * that lives in any of those three cannot be captured at all, which is where the black
     * cells at the horizon were found.
     */
    int headless_jitter;
    // Explicit camera pose, for reproducing a framing exactly. Degrees on the command line,
    // radians here. Each is independent: eye alone re-places the default look direction's
    // origin, which is usually what a report means.
    vec3 cam_eye, cam_target, cam_up;
    int cam_eye_set, cam_target_set, cam_up_set;
    float fov;          // vertical, radians; 0 = the app's own framing
    int player;         // first-person walker instead of the orbit camera
    float walk_speed;   // units/s on the flat; 0 = the walker's own default
    float look_rate;    // radians/s of head turn; 0 = the walker's own default
    int arrows_upright; // up arrow looks UP; the walker's default is inverted
} TreeArgs;

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -x, --headless          Run with a hidden window (for capture / CI)\n");
    printf("  -f, --frames N          Exit after N frames\n");
    printf("  -S, --screenshot PATH   Write the final frame as a binary PPM\n");
    printf("      --screenshot-every N  Also write every Nth frame\n");
    printf("  -W, --width N           Window width (default %u)\n", WIDTH);
    printf("  -H, --height N          Window height (default %u)\n", HEIGHT);
    printf("      --seed N            Tree seed\n");
    printf("      --sun-elevation D   Sun elevation in degrees\n");
    printf("      --sun-azimuth D     Sun azimuth in degrees\n");
    printf("      --no-shadows        Disable the shadow pass\n");
    printf("      --no-fog            Disable the volumetric fog\n");
    printf("      --no-falling-leaves Disable the falling-leaf particles\n");
    printf("      --no-water          Dry land: drop the sea around the island\n");
    printf("      --water-level D     Still-water world Y (default %.1f)\n",
           (double)ground_shore_height());
    printf("      --gerstner-waves    Closed-form octaves instead of spectral cascades\n");
    printf("      --no-water-wetness  The swash leaves the sand exactly as it found it\n");
    printf("      --no-water-film     Drop the swash solver; the closed-form run-up drives\n");
    printf("      --water-foam-debug N  Foam as a binary mask: 1 the crest band after erosion,\n");
    printf("                          2 before it, 3 breaking alone\n");
    printf("      --no-water-surf     No incident wave at the shore: no run-up, no bore\n");
    printf("      --wind-speed M      Spectral wind sea: wind in m/s (default 6)\n");
    printf("      --fetch M           Spectral wind sea: fetch in metres (default 15000)\n");
    printf("      --swell S           Swell train weight, 0 = no swell (default 1; this\n");
    printf("                          app's own swell is 3 m/s over 40 km)\n");
    printf("      --render-mode N     Debug view; 10 = HDR hotspots, 12 = extrapolation\n");
    printf("      --msaa N            MSAA samples (default 4); 1 has no partial coverage\n");
    printf("      --headless-jitter   Keep TAA jitter and the 0.70 render scale headless:\n");
    printf("                          what the window draws, but NOT deterministic\n");
    printf("      --cam-eye x,y,z     Pin the camera position (exact-repro framing)\n");
    printf("      --cam-target x,y,z  Pin what it looks at\n");
    printf("      --cam-up x,y,z      Pin the up vector\n");
    printf("      --fov D             Vertical field of view, DEGREES\n");
    printf("      --player            Walk the island. WASD moves, ARROWS turn the head,\n");
    printf("                          Shift runs, Space jumps. Keyboard only -- the mouse\n");
    printf("                          never moves the camera, so the GUI stays clickable.\n");
    printf("                          With --cam-eye, spawn at its x,z\n");
    printf("      --walk-speed U      Units/s on the flat (default %.0f; implies --player)\n",
           (double)PLAYER_WALK_SPEED);
    printf("      --look-rate D       Head turn, DEGREES/s (default %.0f; implies --player)\n",
           (double)(PLAYER_LOOK_RATE * 180.0f / (float)M_PI));
    printf("      --no-invert-arrows  Up arrow looks UP; the default is inverted (pitch only,\n");
    printf("                          never yaw)\n");
    printf("  -h, --help              This message\n");
}

static bool parse_args(int argc, char** argv, TreeArgs* a) {
    memset(a, 0, sizeof(*a));
    // AFTER the memset, obviously, and not before it. <0 means "untouched", because 0 is
    // itself a legal request -- a scene asking for no swell train at all.
    a->swell = -1.0f;
    a->width = (int)WIDTH;
    a->height = (int)HEIGHT;
    a->seed = 42;
    a->sun_elevation = 0.8f;
    a->sun_azimuth = 193.0f;
    // 0 is a legal water level -- it is the dome's summit -- so the unset value has
    // to sit outside every plausible one.
    a->water_level = -9999.0f;

    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        bool has_next = (i + 1) < argc;

        if (!strcmp(s, "-x") || !strcmp(s, "--headless")) {
            a->headless = 1;
        } else if ((!strcmp(s, "-f") || !strcmp(s, "--frames")) && has_next) {
            a->frames = atoi(argv[++i]);
        } else if ((!strcmp(s, "-S") || !strcmp(s, "--screenshot")) && has_next) {
            a->screenshot = argv[++i];
        } else if (!strcmp(s, "--screenshot-every") && has_next) {
            a->screenshot_every = atoi(argv[++i]);
        } else if ((!strcmp(s, "-W") || !strcmp(s, "--width")) && has_next) {
            a->width = atoi(argv[++i]);
        } else if ((!strcmp(s, "-H") || !strcmp(s, "--height")) && has_next) {
            a->height = atoi(argv[++i]);
        } else if (!strcmp(s, "--seed") && has_next) {
            a->seed = atoi(argv[++i]);
        } else if (!strcmp(s, "--sun-elevation") && has_next) {
            a->sun_elevation = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--sun-azimuth") && has_next) {
            a->sun_azimuth = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--no-water")) {
            a->no_water = 1;
        } else if (!strcmp(s, "--water-level") && has_next) {
            a->water_level = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--gerstner-waves")) {
            a->gerstner_waves = 1;
        } else if (!strcmp(s, "--no-water-wetness")) {
            a->no_water_wetness = 1;
        } else if (!strcmp(s, "--no-water-film")) {
            a->no_water_film = 1;
        } else if (!strcmp(s, "--water-foam-debug") && has_next) {
            a->water_foam_debug = atoi(argv[++i]);
        } else if (!strcmp(s, "--no-water-foam-history")) {
            a->no_water_foam_history = 1;
        } else if (!strcmp(s, "--no-water-surf")) {
            a->no_water_surf = 1;
        } else if (!strcmp(s, "--wind-speed") && has_next) {
            a->wind_speed = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--fetch") && has_next) {
            a->fetch = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--swell") && has_next) {
            a->swell = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--render-mode") && has_next) {
            a->render_mode = atoi(argv[++i]);
        } else if (!strcmp(s, "--msaa") && has_next) {
            a->msaa = atoi(argv[++i]);
        } else if (!strcmp(s, "--headless-jitter")) {
            a->headless_jitter = 1;
        } else if (!strcmp(s, "--player")) {
            a->player = 1;
        } else if (!strcmp(s, "--walk-speed") && has_next) {
            a->walk_speed = (float)atof(argv[++i]);
            a->player = 1;
        } else if (!strcmp(s, "--look-rate") && has_next) {
            // Degrees per second in, radians out, like --fov and the sun angles.
            a->look_rate = (float)atof(argv[++i]) * (float)M_PI / 180.0f;
            a->player = 1;
        } else if (!strcmp(s, "--no-invert-arrows")) {
            a->arrows_upright = 1;
            a->player = 1;
        } else if (!strcmp(s, "--cam-eye") && has_next) {
            a->cam_eye_set = sscanf(argv[++i], "%f,%f,%f", &a->cam_eye[0], &a->cam_eye[1],
                                    &a->cam_eye[2]) == 3;
        } else if (!strcmp(s, "--cam-target") && has_next) {
            a->cam_target_set = sscanf(argv[++i], "%f,%f,%f", &a->cam_target[0],
                                       &a->cam_target[1], &a->cam_target[2]) == 3;
        } else if (!strcmp(s, "--cam-up") && has_next) {
            a->cam_up_set = sscanf(argv[++i], "%f,%f,%f", &a->cam_up[0], &a->cam_up[1],
                                   &a->cam_up[2]) == 3;
        } else if (!strcmp(s, "--fov") && has_next) {
            // Degrees in, radians out: every other angle this app takes on the command line
            // is in degrees, and a lone radian argument is the kind of inconsistency that
            // gets a bug report filed against the renderer.
            a->fov = (float)atof(argv[++i]) * (float)M_PI / 180.0f;
        } else if (!strcmp(s, "--no-shadows")) {
            a->no_shadows = 1;
        } else if (!strcmp(s, "--no-fog")) {
            a->no_fog = 1;
        } else if (!strcmp(s, "--no-falling-leaves")) {
            a->no_falling_leaves = 1;
        } else if (!strcmp(s, "-h") || !strcmp(s, "--help")) {
            print_usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", s);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/*
 * Falling leaves
 *
 * Sparse enough to read as individual leaves rather than weather. They spawn in
 * a box around the canopy, tumble on their own roll, drift downwind, and stop
 * on the island rather than sinking through it.
 */
static void create_falling_leaves(Engine* engine, Scene* scene, float canopy_radius,
                                  float canopy_top) {
    ShaderProgram* particle_prog = create_particle_program();
    if (!particle_prog)
        return;
    add_shader_program_to_engine(engine, particle_prog);

    ParticleSystem* sys = create_particle_system("falling_leaves");
    if (!sys)
        return;
    particle_system_set_backend(sys, create_cpu_particle_sim_backend());

    ParticleEmitter* em = create_particle_emitter("leaf", 256);
    if (!em)
        return;

    ParticleRenderer* pr = create_billboard_particle_renderer(particle_prog);
    // hdr_gain 1.0: these are lit surfaces, not glowing motes -- the mote
    // default of 6.0 would blow them into bloom.
    // The single-leaf sprite, not the cluster atlas: a billboard samples UV
    // 0..1 across its quad, so the atlas would arrive as all its cells crushed
    // into one particle.
    billboard_renderer_set_sprite(pr, leaf_sprite_tex, 1.0f);
    particle_emitter_set_renderer(em, pr);

    leaf_spawn_module = particle_module_spawn_rate(leaf_spawn_rate);
    particle_emitter_add_module(em, leaf_spawn_module);

    vec3 spawn_min = {-canopy_radius, canopy_top * 0.45f, -canopy_radius};
    vec3 spawn_max = {canopy_radius, canopy_top, canopy_radius};
    particle_emitter_add_module(em, particle_module_init_box_location(spawn_min, spawn_max));
    particle_emitter_add_module(em, particle_module_init_lifetime(14.0f, 22.0f));
    particle_emitter_add_module(em, particle_module_init_size(2.0f, 3.5f));
    particle_emitter_add_module(em,
                                particle_module_init_color((vec4){1.0f, 0.85f, 0.55f, 1.0f}, 0.1f));

    particle_emitter_add_module(em, particle_module_update_rotation(0.4f, 1.2f));
    particle_emitter_add_module(em, particle_module_update_curl_noise(0.02f, 5.0f, 0.1f));

    vec3 fall = {scene_wind ? scene_wind->direction[0] * 1.5f : 1.5f, -3.5f,
                 scene_wind ? scene_wind->direction[2] * 1.5f : 0.0f};
    particle_emitter_add_module(em, particle_module_update_drift(fall));
    particle_emitter_add_module(em, particle_module_update_integrate(0.96f));

    // Litter lands ON the dome, which a plane cannot follow: a plane at the crown leaves
    // every leaf outside the trunk floating, and the deeper the island the further. The
    // dome's own curvature-matched sphere does follow it -- to 0.01 units under the canopy
    // and 2 units at the rim -- and is an analytic collider the particle system already has.
    vec3 fit_center = GLM_VEC3_ZERO_INIT;
    float fit_radius = ground_sphere_fit(fit_center);
    particle_emitter_add_module(em,
                                particle_module_collider_sphere(fit_center, fit_radius,
                                                                COLLIDER_KEEP_OUT, 0.0f, 0.0f));

    particle_system_add_emitter(sys, em);
    add_particle_system_to_scene(scene, sys);

    SceneNode* node = create_node();
    set_node_name(node, "falling_leaves");
    set_node_particle_system(node, sys);
    add_child_node(scene->root_node, node);
}

/*
 * Main
 */
int main(int argc, char** argv) {
    TreeArgs args;
    if (!parse_args(argc, argv, &args))
        return 0;

    sun_elevation = args.sun_elevation;
    sun_azimuth = args.sun_azimuth;

    Engine* engine = create_engine("Procedural Tree", args.width, args.height);
    set_engine_headless(engine, args.headless != 0);
    engine->headless_jitter = args.headless_jitter != 0;
    set_engine_screenshot_path(engine, args.screenshot);
    set_engine_screenshot_every(engine, args.screenshot_every);
    set_engine_exit_after_frames(engine, args.frames);
    // TAAU: render the scene at 70% and reconstruct temporally. Set before
    // init_engine, because create_postfx sizes every target from it. Headless
    // drops back to full resolution unless --headless-jitter, since the resolve
    // reconstructs from the jitter and headless suppresses it.
    set_engine_render_scale(engine, 0.70f);
    // Before init_engine so the count is the one the scene target is first built at, rather than
    // a rebuild on the frame after. The engine clamps it to what the driver offers.
    if (args.msaa > 0)
        set_engine_msaa_samples(engine, args.msaa);

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    set_engine_mouse_button_callback(engine, mouse_button_callback);
    set_engine_key_callback(engine, key_callback);

    ShaderProgram* pbr_program = get_engine_shader_program_by_name(engine, "pbr");
    if (!pbr_program) {
        fprintf(stderr, "Failed to get PBR shader\n");
        return -1;
    }
    ShaderProgram* xyz_program = get_engine_shader_program_by_name(engine, "xyz");

    // Camera: low and off-axis so the canopy tops the frame and the low sun
    // rakes its shadows toward the viewer.
    //
    // Overridable, because a look bug in a scene this size is reported as a viewpoint and
    // there was previously no way to hand one over -- the orbit controller's state is not
    // expressible on a command line. Same flags render and forest already carry.
    Camera* camera = create_camera();
    vec3 cam_pos = {140.0f, 95.0f, 600.0f};
    vec3 look_at = {0.0f, 145.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    if (args.cam_eye_set)
        glm_vec3_copy(args.cam_eye, cam_pos);
    if (args.cam_target_set)
        glm_vec3_copy(args.cam_target, look_at);
    if (args.cam_up_set)
        glm_vec3_copy(args.cam_up, up);
    set_camera_position(camera, cam_pos);
    set_camera_look_at(camera, look_at);
    set_camera_up_vector(camera, up);
    set_camera_perspective(camera, args.fov > 0.0f ? args.fov : 0.55f, 2.0f, 3000.0f);
    set_engine_camera(engine, camera);
    camera->distance = glm_vec3_distance(cam_pos, look_at);

    if (args.player) {
        /*
         * Stand where the default camera stands, facing what it faces.
         *
         * Taken from cam_pos rather than authored, so the walker inherits the framing this app
         * was composed around -- tree ahead, sun behind, sea past it -- and keeps inheriting it
         * if that camera moves. Only the BEARING is reused, at 65% of the waterline radius: the
         * camera itself is 616 units out and over open water, and a walker has to start on the
         * island.
         */
        const float bearing = atan2f(cam_pos[0], cam_pos[2]);
        const float spawn_r = GROUND_SHORE_T * GROUND_RADIUS * 0.65f;
        float spawn_x = sinf(bearing) * spawn_r;
        float spawn_z = cosf(bearing) * spawn_r;
        // --cam-eye doubles as "stand here", which is how a walk into the sea gets verified at
        // all: the only other way to reach the water is to hold W, which a headless run cannot.
        // Its Y is ignored -- the ground decides that.
        if (args.cam_eye_set) {
            spawn_x = args.cam_eye[0];
            spawn_z = args.cam_eye[2];
        }
        static Player walker;
        player = &walker;
        // Yaw 0 looks down -Z, so facing the origin from `bearing` is that same angle: the
        // spawn point and the direction home are the same bearing, one negated in Z.
        player_init(player, engine, spawn_x, spawn_z,
                    args.cam_eye_set ? atan2f(spawn_x, spawn_z) : bearing);
        if (args.walk_speed > 0.0f)
            player->walk_speed = args.walk_speed;
        if (args.look_rate > 0.0f)
            player->look_rate = args.look_rate;
        if (args.arrows_upright)
            player->invert_pitch = false;
    } else {
        drag_controller = create_mouse_drag_controller(engine);
    }

    Scene* scene = create_scene();
    SceneNode* root = create_node();
    set_node_name(root, "root");
    set_scene_root_node(scene, root);
    add_scene_to_engine(engine, scene);

    if (xyz_program) {
        set_scene_xyz_shader_program(scene, xyz_program);
    }

    // Textures go through the scene's pool, so the scene must exist first.
    generate_procedural_textures(scene);

    /*
     * Lighting: physically based sky, IBL baked from it, and one sun coupled
     * to the atmosphere. A tree is mostly ambient-lit -- without an environment
     * to sample, every leaf that faces away from the key light goes black.
     */
    sky = create_sky_atmosphere();
    ibl = create_ibl_resources();
    if (sky && ibl) {
        sky->sun_elevation_deg = sun_elevation;
        sky->sun_azimuth_deg = sun_azimuth;
        /*
         * This world's scale, which nothing set until spec 11.44 (the GUI offered a slider
         * for it and no code ever wrote it), so the sky ran at its 1-unit-is-a-metre default
         * while ground.h has always put this app at 22.
         *
         * It is not the sky's alone. Every physical length in the ocean converts through the
         * same number -- the depth a wave shoals over, the distance the short band fades
         * across, the caustic window -- so leaving it wrong gave this app a surf zone 0.38 m
         * wide, which is the hard line at the shore rather than a beach.
         */
        sky->world_units_per_km = GROUND_UNITS_PER_METRE * 1000.0f;
        sky_update_sun_dir(sky);

        if (sky_bake_static_luts(sky, engine) == 0 && sky_bake(sky, ibl, engine) == 0) {
            scene->sky = sky;
            scene->ibl = ibl;
            scene->render_skybox = true;
            scene->skybox_brightness = 1.0f;
            scene->skybox_ground_projection = false;

            sun_light = create_light();
            set_light_name(sun_light, "sun");
            set_light_type(sun_light, LIGHT_DIRECTIONAL);
            set_light_cast_shadows(sun_light, true);
            // Emitter size drives the PCSS penumbra: contact shadows stay
            // crisp under the canopy and soften further from the caster.
            set_light_size(sun_light, 6.0f, 6.0f);
            sky->sun_light = sun_light;
            sky->sun_base_intensity = 10.0f;
            sky_apply_sun_to_light(sky);
            add_light_to_scene(scene, sun_light);

            SceneNode* sun_node = create_node();
            set_node_name(sun_node, "sun");
            set_node_light(sun_node, sun_light);
            add_child_node(root, sun_node);

            printf("Sky: sun at elevation %.1f azimuth %.1f\n", sky->sun_elevation_deg,
                   sky->sun_azimuth_deg);
        }
    }

    // Shadows. Both bounds have to span the GROUND, not the tree -- sized to the
    // tree (300 / 1200) the map ended mid-shadow and left a hard straight edge
    // across the island.
    //
    // far_plane is the one that actually bit, and it is the less obvious of the
    // two: compute_directional_light_space_matrix puts the light's eye at
    // far_plane/2 back and gives the ortho a depth range of far_plane, so the
    // map spans only +/-far_plane/2 ALONG THE LIGHT. At 1200 that is +/-600
    // against an 1800-unit island, and everything past it projects outside the
    // map and reads unshadowed -- a plane perpendicular to the light, which is
    // why the edge is straight. ortho_size was undersized too, but fixing it
    // alone barely moved the artifact.
    //
    // The requirement is roughly 2 * (ground radius + tree height / tan(sun
    // elevation)): the shadow has to fit along the light, and it lengthens fast
    // as the sun drops. At the 620-unit radius and a 250-unit tree that is 4,800
    // at the default 8 degrees, 5,997 at 6, and 6,956 at 5 -- so 6000 holds the
    // slider to about 6, and below that the shadow is longer than the island and
    // runs off it regardless. The TREE-HEIGHT term dominates, which is why
    // shrinking the radius from 900 in spec 11.35 barely moved any of these.
    ShadowSystem* ss = scene->shadow_system;
    if (ss) {
        ss->enabled = args.no_shadows == 0;
        ss->ortho_size = GROUND_RADIUS;
        ss->near_plane = 0.1f;
        ss->far_plane = 6000.0f;
        ss->pcss_enabled = true;
        ss->pcss_softness = 1.5f;
        ss->cascade_count = SHADOW_CASCADES;
    }

    /*
     * Wind. The tree's materials opt in per-mode; the island stays rigid.
     */
    scene_wind = create_wind("breeze");
    glm_vec3_copy((vec3){1.0f, 0.0f, 0.35f}, scene_wind->direction);
    scene_wind->strength = 2.5f;
    scene_wind->speed = 1.2f;
    scene_wind->gust_frequency = 0.35f;
    scene_wind->gust_amount = 0.55f;
    scene_wind->turbulence = 0.5f;
    set_scene_wind(scene, scene_wind);

    /*
     * Materials
     *
     * None of these may take an AO texture: the PBR shader reads UV1 as the AO
     * map's UV, and UV1 on the tree meshes carries wind data.
     */
    // Named so the material editor can address them. Procedural materials get no name
    // from an importer, and an unnamed row cannot be told apart from the four beside it.
    bark_material = create_material();
    bark_material->name = safe_strdup("bark");
    glm_vec3_one(bark_material->albedo);
    bark_material->roughness = 1.0f; // the map carries it (factor x map)
    bark_material->metallic = 0.0f;
    bark_material->ao = 1.0f;
    bark_material->wind_response = 1.0f;
    bark_material->wind_mode = 1; // vegetation branch
    bark_material->parallax_scale = 0.03f;
    set_material_shader_program(bark_material, pbr_program);
    set_material_albedo_tex(bark_material, bark_albedo_tex);
    set_material_normal_tex(bark_material, bark_normal_tex);
    set_material_roughness_tex(bark_material, bark_roughness_tex);
    set_material_height_tex(bark_material, bark_height_tex);

    leaf_material = create_material();
    leaf_material->name = safe_strdup("leaf");
    glm_vec3_one(leaf_material->albedo);
    // 1.0 because the roughness map is authoritative: the shader multiplies
    // factor by map, so any factor below 1 darkens the whole range and pushes
    // the canopy glossy enough to mirror the sky.
    leaf_material->roughness = 1.0f;
    leaf_material->metallic = 0.0f;
    leaf_material->ao = 1.0f;
    // Alpha-masked cutout, drawn from both sides, and -- unlike hair cards --
    // allowed into the shadow map, which is what dapples the ground.
    leaf_material->alpha_mode = ALPHA_MASK;
    leaf_material->alphaCutoff = 0.4f;
    leaf_material->doubleSided = true;
    leaf_material->foliage_shadows = 1;
    leaf_material->wind_response = 1.0f;
    leaf_material->wind_mode = 2; // vegetation leaf (adds flutter)
    // Thin leaves transmit light: without this the canopy reads as opaque
    // plastic whenever the sun is behind it.
    leaf_material->subsurface = 0.6f;
    set_material_shader_program(leaf_material, pbr_program);
    set_material_albedo_tex(leaf_material, leaf_albedo_tex);
    set_material_normal_tex(leaf_material, leaf_normal_tex);
    set_material_roughness_tex(leaf_material, leaf_roughness_tex);

    if (engine->postfx) {
        postfx_reset_sss_profiles(engine->postfx);
        leaf_material->subsurface_profile =
            postfx_add_sss_profile(engine->postfx, (vec3){0.45f, 0.75f, 0.2f}, 0.25f);
    }
    apply_season(season);
    prev_season = season;

    island_material = create_material();
    island_material->name = safe_strdup("island");
    glm_vec3_one(island_material->albedo);
    island_material->roughness = 0.9f;
    island_material->metallic = 0.0f;
    island_material->ao = 1.0f;
    set_material_shader_program(island_material, pbr_program);
    set_material_albedo_tex(island_material, island_albedo_tex);
    set_material_normal_tex(island_material, island_normal_tex);
    set_material_roughness_tex(island_material, island_roughness_tex);
    // The beach remembers the swash. Full response: this IS the sand the waves run over, and
    // the run-up bounds itself, so there is nothing here to scale down.
    island_material->shore_wetness = 1.0f;
    /*
     * Sample the sand stochastically, so the tile stops being recognisable.
     *
     * The UVs run 0 to 40 across the island, so one UV unit is one tile, and a lattice cell of
     * that order gives each tile-sized patch of ground its own offset into the texture. Gated
     * on the transform having actually happened -- without the table the shader would undo a
     * transform the map never had.
     */
    if (sand_stochastic_ready) {
        island_material->stochastic_scale = 1.0f;
        memcpy(island_material->stochastic_lut, sand_stochastic_lut,
               sizeof(sand_stochastic_lut));
    }

    /*
     * The seabed shares the island's sand maps, and now its vertex colours too, so the two
     * meshes meet at the rim as one surface rather than as two materials that happen to
     * touch. The bed's own albedo factor stays white for the same reason the island's does:
     * the colour is per-vertex, and a factor here would tint the beach as well.
     *
     * Rougher than the beach, and nothing else -- opaque, no wind, no subsurface.
     */
    seabed_material = create_material();
    seabed_material->name = safe_strdup("seabed");
    glm_vec3_one(seabed_material->albedo);
    seabed_material->roughness = 0.95f;
    seabed_material->metallic = 0.0f;
    seabed_material->ao = 1.0f;
    set_material_shader_program(seabed_material, pbr_program);
    set_material_albedo_tex(seabed_material, island_albedo_tex);
    set_material_normal_tex(seabed_material, island_normal_tex);
    set_material_roughness_tex(seabed_material, island_roughness_tex);
    // Wetted too, and not optionally: the two meshes share the rim, so a wet island against a
    // dry seabed would seam exactly where they are meant to be one surface.
    seabed_material->shore_wetness = 1.0f;

    // Wet grey stone. Untextured on purpose: the boulders are small in frame and half of
    // them are under water, where a texture buys nothing a colour and a roughness do not.
    rock_material = create_material();
    rock_material->name = safe_strdup("shore_rock");
    glm_vec3_copy((vec3){0.20f, 0.19f, 0.18f}, rock_material->albedo);
    rock_material->roughness = 0.86f;
    rock_material->metallic = 0.0f;
    rock_material->ao = 1.0f;
    set_material_shader_program(rock_material, pbr_program);

    // Grass. Opaque, so it casts and receives shadows with no special handling
    // -- the canopy dapple landing on it is the point of having it. Colour is
    // entirely per-vertex, so no textures and no AO map to collide with UV1.
    grass_material = create_material();
    grass_material->name = safe_strdup("grass");
    glm_vec3_one(grass_material->albedo);
    grass_material->roughness = 0.78f;
    grass_material->metallic = 0.0f;
    grass_material->ao = 1.0f;
    grass_material->doubleSided = true;
    // Grass is far more mobile than wood.
    grass_material->wind_response = 1.7f;
    grass_material->wind_mode = 2; // vegetation leaf: sway plus tip flutter
    // Thin blades glow when the sun is behind them, like the leaves.
    grass_material->subsurface = 0.45f;
    glm_vec3_copy((vec3){0.45f, 0.70f, 0.18f}, grass_material->subsurface_color);
    // A real profile slot is not optional once subsurface is non-zero: the
    // shader tags the skin-diffuse buffer with profile + 1, so an unassigned
    // -1 writes the tag reserved for "not a subsurface surface". The blur then
    // skips those pixels while their diffuse is still sitting in the buffer,
    // and the unblurred energy composites back as blown-out speckle.
    /*
     * SHARES the leaf's profile rather than registering a second one, and that is a
     * correctness choice, not a saving.
     *
     * The profile tag is written into the skin-diffuse buffer's alpha and that buffer is
     * MSAA-resolved -- a box filter over a CATEGORICAL value. With two tags in the frame a
     * partly covered pixel resolves to their mean, and the mean of tag 2 and the uncovered 0
     * is tag 1: grass and petals, which are thin enough that most of their pixels are
     * silhouette pixels, get blurred with the LEAF profile instead of their own. Wind moves
     * the coverage every frame, so the misfiling flickers -- which is what it looks like.
     *
     * With ONE tag in the frame the same averaging is harmless: a pixel at or above half
     * coverage rounds back to that tag, and below half it rounds to 0 and is simply dropped.
     * Wrong-profile selection stops being expressible.
     *
     * What it costs is the radius distinction, 0.25 against 0.15. The two profiles' colours
     * were already within 0.05 of each other, so that is the whole difference. The library
     * fix -- the tag in stencil, which is integer and per-sample -- is spec 11.37 phase 2,
     * and this goes back to two profiles once that lands.
     */
    grass_material->subsurface_profile = leaf_material->subsurface_profile;
    set_material_shader_program(grass_material, pbr_program);

    // Clamped rather than trusted: an out-of-range mode reaches the shader as an integer that
    // matches no branch, which shades nothing and reads as a black frame rather than as a bad
    // argument.
    if (args.render_mode > 0) {
        engine->current_render_mode =
            (RenderMode)(args.render_mode > RENDER_MODE_EXTRAPOLATION ? RENDER_MODE_EXTRAPOLATION
                                                                      : args.render_mode);
    }

    create_island(root);

    // The sea around it, and the bed under the sea. Both after the island because the level is
    // derived from the dome's own constants, and the bed provider is the same function the
    // island mesh and the grass root themselves with -- so the shoreline cannot drift from the
    // ground it meets. Under ONE guard because they are one feature: a seabed with no sea over
    // it is a plate around a dome, which is the saucer --no-water exists to avoid.
    if (!args.no_water) {
        create_seabed(root);
        // Boulders straddling the waterline. Under the same guard: they are placed by DEPTH,
        // and with no sea there is no depth to place them by -- a scatter of rocks at
        // arbitrary radii on dry land is not what this is for.
        create_shore_rocks(root);
        Water* water = create_water();
        if (water) {
            // The still level comes from ground_shore_height and NOT from sampling
            // ground_height_at at some bearing, which is a WOBBLED height: the sea would then
            // sit at whatever the shore happened to be doing along +x, and the beach banding
            // -- which reads the unwobbled level -- would band against a different waterline
            // than the one drawn.
            water->level =
                args.water_level > -9000.0f ? args.water_level : ground_shore_height();
            water->extent = TREE_WATER_EXTENT;
            water->height_at = tree_bed_height;
            /*
             * SPECTRAL, not the library's Gerstner default, and the sea is why (spec 11.35).
             *
             * Gerstner is four octaves off one wind direction, so at grazing incidence -- which
             * is most of this frame -- it reads as corduroy: parallel bands marching to the
             * horizon. The fan (`spread` below) hides that near the camera and cannot far away,
             * because the footprint filtering leaves only the longest octave out there and one
             * sinusoid is one sinusoid however it is aimed. A directional spectrum has no
             * preferred phase to line up, so it does not do this.
             *
             * The library default stays Gerstner because it allocates nothing, which is right
             * where water is incidental. Here the sea IS the frame, and 45 ping-pong draws
             * against that is a trade worth making. --gerstner-waves is the A/B, and the only
             * way to reach the four wave parameters below: the spectral path ignores them
             * because its sea state comes out of a wind speed and a fetch instead.
             */
            water->wave_model = args.gerstner_waves ? WATER_WAVES_GERSTNER : WATER_WAVES_FFT;
            water->foam_debug = (WaterFoamDebug)args.water_foam_debug;
            if (args.no_water_foam_history)
                water->foam_history = false;
            if (args.no_water_surf)
                water->surf = false;
            /*
             * THE SEA STATE, authored here rather than inherited.
             *
             * create_water's defaults are a fully-developed 11.5 m/s sea over 120 km of
             * fetch -- about 2 m significant height. That is a real sea state and the
             * library is right to default to it, but this island is 28 m across: a 2 m
             * swell breaks on the whole of it at once, so the depth-limited surf term
             * covered the shore in one unbroken sheet of whitewater and the crest term
             * scattered whitecaps over the lagoon. The scene was asking for a storm.
             *
             * 6 m/s over 15 km is a light breeze on a sheltered lagoon, which is the water
             * this scene is otherwise painted as -- turquoise, shallow, calm. The surf
             * becomes a line at the beach instead of a field over the bay.
             */
            water->sea.wind_sea.wind_speed = 6.0f;
            water->sea.wind_sea.fetch = 15000.0f;
            /*
             * AND ITS SWELL, for the same reason and to the same scale.
             *
             * The library's swell is 8.4 m/s over 310 km. That is an ocean swell, and on
             * this scene it carried about 1.1 m of significant height -- three times the
             * wind sea above. Until spec 11.48 it was not sayable, so lowering the wind
             * left it standing: the shelf met a metre of swell whatever the breeze did, and
             * since breaking is depth-limited it broke over the whole bay at once.
             *
             * 3 m/s over 40 km is 0.16 m here against the wind sea's 0.64 -- a swell that
             * is present rather than dominant, which is what a sheltered lagoon has. Tuned
             * by eye against the A/B, not to a target number: with no swell at all a single
             * train's crests read as corduroy, so zero is the wrong answer too.
             */
            water->sea.swell.wind_speed = 3.0f;
            water->sea.swell.fetch = 40000.0f;
            if (args.wind_speed > 0.0f)
                water->sea.wind_sea.wind_speed = args.wind_speed;
            if (args.fetch > 0.0f)
                water->sea.wind_sea.fetch = args.fetch;
            if (args.swell >= 0.0f)
                water->sea.swell.scale = args.swell;
            if (args.no_water_wetness)
                water->wetness = false;
            if (args.no_water_film)
                water->film = false;
            // Shorter and livelier than the swell 11.32 had to settle for, when a
            // world-space grid put a 17-unit cell here and anything under ~150 units read
            // as facets. The projected grid sizes its cells in pixels instead, so what a
            // wavelength has to survive is no longer the extent -- it is the footprint
            // where the eye is looking, and near the shore that is well under a unit.
            water->wavelength = 45.0f;
            water->amplitude = 0.85f;
            water->steepness = 0.55f;
            // Wide fan: four octaves off one direction at this scale print as
            // corduroy, and the sea is most of the frame.
            water->spread = 0.85f;
            /*
             * Optical properties. `absorption` is extinction per WORLD UNIT, and the
             * library's default (0.45, 0.09, 0.06) is clear seawater per METRE -- correct
             * as it stands only where a unit IS a metre. This world is 22 units to it, so
             * the default is DIVIDED rather than re-typed at this scale: the numerator
             * stays in one place and cannot drift from the value it means.
             *
             * Taken per metre it was a sight line of about a metre, so nothing under the
             * surface could be seen from anywhere.
             *
             * SCATTER is not divided. It is the colour the absorbed energy comes back as
             * -- a radiance, with no length in it to convert. It is also where the lagoon
             * lives: greener than the library's open-ocean blue, against an extinction
             * that is plain clear seawater.
             */
            glm_vec3_divs(water->absorption, GROUND_UNITS_PER_METRE, water->absorption);
            glm_vec3_copy((vec3){0.03f, 0.13f, 0.14f}, water->scatter);
            scene->water = water;
        }
    }

    /*
     * Post-processing: a film look rather than the engine defaults, on PBR
     * Neutral so foliage colour stays faithful rather than being pushed.
     */
    PostFX* fx = engine->postfx;
    if (fx) {
        fx->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;
        postfx_apply_film_look(fx);
        fx->grain_strength = 0.015f;

        // TAAU is a temporal reconstruction, so the render scale above does
        // nothing without this: the seam only dispatches when the resolve runs,
        // and unset it would render at 70% and simply magnify.
        fx->taa_enabled = true;

        fx->fog_enabled = false;
        fx->fog_density = 0.0005f;
        fx->fog_height_falloff = 75.0f;
        fx->fog_floor_y = 0.0f;
        fx->fog_far = 800.0f;

        fx->dof_enabled = false;
        fx->dof_autofocus = true;
        fx->dof_focus_distance = 620.0f;
        fx->dof_focus_range = 320.0f;

        if (fx->ssao_radius < 1.6f)
            fx->ssao_radius = 1.6f;
        fx->contact_shadows_enabled = true;
        fx->ssr_enabled = true;
        fx->ssgi_enabled = true;
    }
    engine->oit_enabled = true;

    // Tree shape
    params.seed = args.seed;
    params.max_depth = 4;
    // A tall, upright habit: a long trunk, branches held closer to vertical,
    // and a stronger pull toward the light, which narrows the crown rather
    // than letting it spread into a ball.
    params.trunk_length = 125.0f;
    params.trunk_radius = 9.0f;
    params.branches_per_node = 3;
    params.length_decay = 0.70f;
    params.taper = 0.62f;
    params.branch_angle = 27.0f;
    params.angle_variance = 12.0f;
    params.twist = 137.5f;
    params.droop = 0.32f;
    params.curve_noise = 0.4f;
    params.phototropism = 0.45f;
    params.lateral_density = 1.0f;
    params.twig_scale = 1.0f;
    params.show_leaves = 1;
    // A card carries a whole sprig, so it is sized as one and spaced sparsely:
    // the canopy should show its branch structure through the foliage.
    params.leaf_size = 15.0f;
    params.leaf_density = 1.3f;

    // Grass field
    grass_params.seed = args.seed;
    grass_params.radius = 130.0f;
    grass_params.clear_radius = 11.0f;
    grass_params.density = 5.5f;
    grass_params.patchiness = 0.55f;
    grass_params.height = 5.5f;
    grass_params.blade_width = 0.55f;
    grass_params.bend = 0.42f;
    grass_params.flower_amount = 0.02f;
    grass_params.seed_head_amount = 0.09f;

    // The tree node is created once and outlives every rebuild; regeneration
    // only swaps its meshes, so nothing else parented to the root is at risk.
    tree_root = create_node();
    set_node_name(tree_root, "tree");
    add_child_node(root, tree_root);

    grass_node = create_node();
    set_node_name(grass_node, "grass");
    add_child_node(root, grass_node);
    regenerate_grass(&grass_params);
    memcpy(&prev_grass_params, &grass_params, sizeof(GrassParams));

    // Build once here so the canopy bounds are known before the leaf emitter
    // is sized; the render callback picks up any later slider change.
    regenerate_tree(&params);
    memcpy(&prev_params, &params, sizeof(TreeParams));

    if (!args.no_falling_leaves) {
        float canopy_top = 200.0f;
        float canopy_radius = 110.0f;
        if (tree_root && tree_root->mesh_count > 0) {
            canopy_top = tree_root->meshes[0]->aabb.max[1];
            float rx = fmaxf(fabsf(tree_root->meshes[0]->aabb.min[0]),
                             fabsf(tree_root->meshes[0]->aabb.max[0]));
            float rz = fmaxf(fabsf(tree_root->meshes[0]->aabb.min[2]),
                             fabsf(tree_root->meshes[0]->aabb.max[2]));
            canopy_radius = fmaxf(rx, rz);
        }
        create_falling_leaves(engine, scene, canopy_radius, canopy_top);
    }

    set_engine_show_gui(engine, !args.headless);
    set_engine_show_fps(engine, !args.headless);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);

    engine_run(engine, NULL, render_scene_callback);

    printf("Cleaning up...\n");
    free_mouse_drag_controller(drag_controller);
    // The scene owns the wind, sky, and IBL; free_engine takes them with it.
    free_engine(engine);

    printf("Goodbye!\n");
    return 0;
}
