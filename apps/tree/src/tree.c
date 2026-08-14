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
#include "cetra/procedural/vegetation_tex.h"
#include "ground.h"
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

/*
 * Generate island/ground normal texture (mostly flat with some variation)
 */
static unsigned char* generate_island_normal(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    veg_noise_seed(1000);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            float nx = (float)x / width * 16.0f;
            float ny = (float)y / height * 16.0f;

            // Subtle height variation for normal calculation
            float h = veg_fbm2(nx, ny, 3, 0.5f) * 0.1f;
            float hx = veg_fbm2(nx + 0.1f, ny, 3, 0.5f) * 0.1f;
            float hy = veg_fbm2(nx, ny + 0.1f, 3, 0.5f) * 0.1f;

            // Derive normal from height differences. Tangent space puts the
            // surface normal on +Z, as the bark map does -- writing it on +Y
            // (the world-space convention) aims every ground texel sideways
            // along its bitangent, and the ground then never faces the sun.
            float dx = (hx - h) * 2.0f;
            float dy = (hy - h) * 2.0f;

            vec3 normal = {-dx, -dy, 1.0f};
            glm_vec3_normalize(normal);

            // Convert to 0-255 range
            data[idx] = (unsigned char)((normal[0] * 0.5f + 0.5f) * 255);
            data[idx + 1] = (unsigned char)((normal[1] * 0.5f + 0.5f) * 255);
            data[idx + 2] = (unsigned char)((normal[2] * 0.5f + 0.5f) * 255);
        }
    }

    return data;
}

/*
 * Generate island/ground albedo texture
 */
static unsigned char* generate_island_albedo(int width, int height) {
    unsigned char* data = malloc(width * height * 3);
    if (!data)
        return NULL;

    veg_noise_seed(999);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            // Base noise for variation
            float nx = (float)x / width * 8.0f;
            float ny = (float)y / height * 8.0f;

            float noise = veg_fbm2(nx, ny, 4, 0.5f);
            float detail = veg_fbm2(nx * 4.0f, ny * 4.0f, 2, 0.5f) * 0.3f;

            float combined = noise + detail;

            // Earth/dirt brown base
            float r = 0.35f + combined * 0.15f;
            float g = 0.25f + combined * 0.12f;
            float b = 0.15f + combined * 0.08f;

            // Add some green patches (grass)
            float grass = veg_fbm2(nx * 2.0f + 100.0f, ny * 2.0f, 3, 0.6f);
            if (grass > 0.3f) {
                float grass_blend = (grass - 0.3f) * 1.5f;
                grass_blend = fminf(grass_blend, 0.6f);
                r = r * (1.0f - grass_blend) + 0.2f * grass_blend;
                g = g * (1.0f - grass_blend) + 0.4f * grass_blend;
                b = b * (1.0f - grass_blend) + 0.15f * grass_blend;
            }

            data[idx] = (unsigned char)(fminf(fmaxf(r, 0.0f), 1.0f) * 255);
            data[idx + 1] = (unsigned char)(fminf(fmaxf(g, 0.0f), 1.0f) * 255);
            data[idx + 2] = (unsigned char)(fminf(fmaxf(b, 0.0f), 1.0f) * 255);
        }
    }

    return data;
}

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

    printf("Generating procedural island textures...\n");
    island_albedo_tex =
        bake_texture(scene, generate_island_albedo(T, T), T, T, 3, true, "proc_island_albedo");
    island_normal_tex =
        bake_texture(scene, generate_island_normal(T, T), T, T, 3, false, "proc_island_normal");

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

static float sun_elevation = 14.0f;
static float sun_azimuth = 235.0f;

/*
 * Mouse drag controller
 */
static MouseDragController* drag_controller = NULL;

/*
 * The sea (spec 11.32). ON by default here, unlike `render` and `forest`, because
 * this app's ground is not a landscape that happens to end -- it is a dome, and a
 * dome surrounded by nothing reads as a saucer floating in the sky. `--no-water`
 * returns the old dry framing.
 *
 * The level is DERIVED from the dome, not picked: over y = H*(1 - t^2) - H a level of
 * -H*t^2 puts the waterline at exactly t*GROUND_RADIUS, so changing the island's shape
 * moves the sea to match instead of needing it re-tuned. See ground.h for the shape and for
 * why GROUND_SHORE_T is where it is.
 *
 * The wave train is scaled to THIS app's units, not carried over from the water
 * fixture. A trunk here is 125 units, so the fixture's 6-unit wavelength would be
 * invisible.
 */
#define TREE_WATER_LEVEL (-GROUND_HEIGHT * GROUND_SHORE_T * GROUND_SHORE_T)
// The SHOALING BED's domain, and nothing else -- since spec 11.34 the grid is projected from
// the frustum and reaches the horizon at any extent. So this is sized for the SHORE BAND
// rather than for the island: tight enough that bed texels land on the shoal ramp (3.1 units
// per texel here, against a ramp 8.4 units wide), and clear of the waterline at 310 on both
// sides. Outside it the bed field reads its edge, which is open water -- correct, because the
// per-fragment water column comes from the depth buffer and not from here.
#define TREE_WATER_EXTENT 400.0f

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
    ground_build_mesh(mesh, 24, 64, 40.0f);
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
 * Create the seabed (spec 11.34 phase 6).
 *
 * Authored in WORLD space, unlike the island, which is built about its crown and translated:
 * the seabed's whole job is to continue the island's flank, and reading ground_height_at
 * directly is how it shares that edge rather than approaching it.
 */
static void create_seabed(SceneNode* parent) {
    Mesh* mesh = create_mesh();
    if (!ground_build_seabed(mesh, 40, 96, 60.0f)) {
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

        igSeparatorText("Sun");
        bool sun_moved = igSliderFloat("Elevation", &sun_elevation, -5.0f, 89.0f, "%.1f", 0);
        sun_moved |= igSliderFloat("Azimuth", &sun_azimuth, 0.0f, 360.0f, "%.1f", 0);
        if (sun_moved && sky) {
            sky->sun_elevation_deg = sun_elevation;
            sky->sun_azimuth_deg = sun_azimuth;
            sky_update_sun(sky, ibl, (Engine*)engine);
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

    // Update camera - only if not hovering over GUI. Deliberately the wall
    // clock, not the frame clock: drag damping is input response.
    if (drag_controller && app_can_process_3d_input(engine)) {
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
    // Explicit camera pose, for reproducing a framing exactly. Degrees on the command line,
    // radians here. Each is independent: eye alone re-places the default look direction's
    // origin, which is usually what a report means.
    vec3 cam_eye, cam_target, cam_up;
    int cam_eye_set, cam_target_set, cam_up_set;
    float fov; // vertical, radians; 0 = the app's own framing
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
           (double)TREE_WATER_LEVEL);
    printf("      --gerstner-waves    Closed-form octaves instead of spectral cascades\n");
    printf("      --cam-eye x,y,z     Pin the camera position (exact-repro framing)\n");
    printf("      --cam-target x,y,z  Pin what it looks at\n");
    printf("      --cam-up x,y,z      Pin the up vector\n");
    printf("      --fov D             Vertical field of view, DEGREES\n");
    printf("  -h, --help              This message\n");
}

static bool parse_args(int argc, char** argv, TreeArgs* a) {
    memset(a, 0, sizeof(*a));
    a->width = (int)WIDTH;
    a->height = (int)HEIGHT;
    a->seed = 42;
    a->sun_elevation = 14.0f;
    a->sun_azimuth = 235.0f;
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
    set_engine_screenshot_path(engine, args.screenshot);
    set_engine_screenshot_every(engine, args.screenshot_every);
    set_engine_exit_after_frames(engine, args.frames);
    // TAAU: render the scene at 70% and reconstruct temporally. Set before
    // init_engine, because create_postfx sizes every target from it. Headless
    // drops back to full resolution unless --headless-jitter, since the resolve
    // reconstructs from the jitter and headless suppresses it.
    set_engine_render_scale(engine, 0.70f);

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

    drag_controller = create_mouse_drag_controller(engine);

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
    // as the sun drops -- 3400 at the default 14 degrees, 4100 at 10, 5600 at 6.
    // 6000 holds the slider down to about 5 degrees; below that the shadow is
    // longer than the island and runs off it regardless.
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
    bark_material = create_material();
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
    leaf_material->foliage_shadows = true;
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
    glm_vec3_one(island_material->albedo);
    island_material->roughness = 0.9f;
    island_material->metallic = 0.0f;
    island_material->ao = 1.0f;
    set_material_shader_program(island_material, pbr_program);
    set_material_albedo_tex(island_material, island_albedo_tex);
    set_material_normal_tex(island_material, island_normal_tex);

    // The seabed reuses the island's textures with a darker, cooler albedo rather than a
    // third procedural generator: it is only ever seen through metres of water, which is a
    // colour filter strong enough that the difference a bespoke texture would make does not
    // survive it. Rougher than the beach, and nothing else -- opaque, no wind, no subsurface.
    seabed_material = create_material();
    glm_vec3_copy((vec3){0.34f, 0.36f, 0.33f}, seabed_material->albedo);
    seabed_material->roughness = 0.95f;
    seabed_material->metallic = 0.0f;
    seabed_material->ao = 1.0f;
    set_material_shader_program(seabed_material, pbr_program);
    set_material_albedo_tex(seabed_material, island_albedo_tex);
    set_material_normal_tex(seabed_material, island_normal_tex);

    // Grass. Opaque, so it casts and receives shadows with no special handling
    // -- the canopy dapple landing on it is the point of having it. Colour is
    // entirely per-vertex, so no textures and no AO map to collide with UV1.
    grass_material = create_material();
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
    if (engine->postfx)
        grass_material->subsurface_profile =
            postfx_add_sss_profile(engine->postfx, (vec3){0.40f, 0.70f, 0.16f}, 0.15f);
    set_material_shader_program(grass_material, pbr_program);

    create_island(root);

    // The sea around it, and the bed under the sea. Both after the island because the level is
    // derived from the dome's own constants, and the bed provider is the same function the
    // island mesh and the grass root themselves with -- so the shoreline cannot drift from the
    // ground it meets. Under ONE guard because they are one feature: a seabed with no sea over
    // it is a plate around a dome, which is the saucer --no-water exists to avoid.
    if (!args.no_water) {
        create_seabed(root);
        Water* water = create_water();
        if (water) {
            water->level =
                args.water_level > -9000.0f ? args.water_level : TREE_WATER_LEVEL;
            water->extent = TREE_WATER_EXTENT;
            water->height_at = tree_bed_height;
            /*
             * SPECTRAL, not the library's Gerstner default, and the sea is why (spec 11.34).
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
            // A lagoon rather than open ocean: a shorter sight line through the
            // body than the library default, so the shallows over the dome's flank
            // read green before they go blue.
            glm_vec3_copy((vec3){0.10f, 0.035f, 0.020f}, water->absorption);
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
