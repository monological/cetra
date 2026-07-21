// Spores - cordyceps spore-room demo, the test bed for the general particle
// system (specs/5.0-particle-system.md, specs/5.1-particle-scene-integration.md).
//
// A dark interior lit by one directional key, with a cordyceps-spore particle
// system (pale green motes on curl-noise turbulence, soft + shadow-lit) attached
// to a scene node -- the engine ticks and renders it, the app just builds and
// attaches. Driven by the game loop (no physics).
//
// Flags: --headless, --frames N, --screenshot PATH (deterministic CI capture).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/material.h"
#include "cetra/scene.h"
#include "cetra/engine.h"
#include "cetra/render.h"
#include "cetra/geometry.h"
#include "cetra/light.h"
#include "cetra/camera.h"
#include "cetra/transform.h"
#include "cetra/app.h"
#include "cetra/game/game.h"
#include "cetra/noise.h"
#include "cetra/particle_system.h"
#include "cetra/particle_module.h"

static ShaderProgram* g_pbr = NULL;
static MouseDragController* g_drag = NULL;
static bool g_use_cpu = false; // --cpu: use the CPU sim backend instead of GPU transform feedback

// A static, unlit-until-the-key-hits-it surface. Geometry is generated at
// `pos` so the node transform can stay identity (matches the shapes app).
static void add_box(SceneNode* root, vec3 pos, vec3 size, vec3 albedo) {
    SceneNode* node = create_node();
    Mesh* mesh = create_mesh();
    Box box;
    glm_vec3_copy(pos, box.position);
    glm_vec3_copy(size, box.size);
    generate_box_to_mesh(mesh, &box);

    Material* mat = create_material();
    glm_vec3_copy(albedo, mat->albedo);
    mat->roughness = 0.9f;
    mat->metallic = 0.0f;
    set_material_shader_program(mat, g_pbr);
    mesh->material = mat;

    add_mesh_to_node(node, mesh);
    add_child_node(root, node);
}

static void add_floor(SceneNode* root, float extent, vec3 albedo) {
    SceneNode* node = create_node();
    Mesh* mesh = create_mesh();
    Plane p = {
        .position = {0, 0, 0}, .width = extent, .depth = extent, .segments_w = 1, .segments_d = 1};
    generate_plane_to_mesh(mesh, &p);

    Material* mat = create_material();
    glm_vec3_copy(albedo, mat->albedo);
    mat->roughness = 0.95f;
    mat->metallic = 0.0f;
    set_material_shader_program(mat, g_pbr);
    mesh->material = mat;

    add_mesh_to_node(node, mesh);
    add_child_node(root, node);
}

static void on_init(Game* game) {
    Engine* engine = game->engine;
    g_pbr = get_engine_shader_program_by_name(engine, "pbr");

    // FPS readout pinned top-right (like the render app). Skipped in headless: the
    // digits change per run and would break screenshot determinism.
    set_engine_show_fps(engine, !engine->headless);

    Scene* scene = create_scene();
    SceneNode* root = create_node();
    set_node_name(root, "root");
    set_scene_root_node(scene, root);
    game_set_scene(game, scene);

    // Dark infected interior: floor + three walls (front open toward camera).
    vec3 wall_albedo = {0.10f, 0.10f, 0.09f};
    vec3 floor_albedo = {0.08f, 0.08f, 0.07f};
    add_floor(root, 24.0f, floor_albedo);
    add_box(root, (vec3){0, 5, -12}, (vec3){24, 10, 0.5f}, wall_albedo); // back
    add_box(root, (vec3){-12, 5, 0}, (vec3){0.5f, 10, 24}, wall_albedo); // left
    add_box(root, (vec3){12, 5, 0}, (vec3){0.5f, 10, 24}, wall_albedo);  // right

    // One warm directional key spilling in from front-top, casting shadows.
    Light* key = create_light();
    set_light_name(key, "key");
    set_light_type(key, LIGHT_DIRECTIONAL);
    set_light_direction(key, (vec3){-0.25f, -0.75f, -0.6f});
    set_light_color(key, (vec3){1.0f, 0.86f, 0.62f});
    set_light_intensity(key, 3.0f);
    set_light_cast_shadows(key, true);
    add_light_to_scene(scene, key);
    SceneNode* key_node = create_node();
    set_node_name(key_node, "key_light");
    set_node_light(key_node, key);
    add_child_node(root, key_node);

    // Room-scale shadows.
    if (scene->shadow_system) {
        scene->shadow_system->ortho_size = 14.0f;
        scene->shadow_system->near_plane = 0.1f;
        scene->shadow_system->far_plane = 60.0f;
        scene->shadow_system->cascade_count = 2;
        scene->shadow_system->pcss_enabled = true;
    }

    // Orbit camera looking into the room.
    Camera* cam = create_camera();
    set_camera_position(cam, (vec3){0.0f, 6.0f, 22.0f});
    set_camera_look_at(cam, (vec3){0.0f, 4.0f, 0.0f});
    set_camera_up_vector(cam, (vec3){0.0f, 1.0f, 0.0f});
    set_camera_perspective(cam, 0.9f, 0.1f, 200.0f);
    set_engine_camera(engine, cam);
    set_engine_camera_mode(engine, CAMERA_MODE_ORBIT);
    cam->distance = 24.0f;

    g_drag = create_mouse_drag_controller(engine);

    upload_buffers_to_gpu_for_nodes(root);

    // Cordyceps-spore particle system: fine pale-green motes on curl-noise
    // turbulence. Attached to a scene node -- the engine ticks + renders it, the
    // app just builds and attaches. The node sits at the origin (identity), so
    // the emitter box is world-space; move the node to move the whole cloud.
    noise_seed(1337u);

    ShaderProgram* particle_prog = create_particle_program();
    add_shader_program_to_engine(engine, particle_prog);

    // GPU transform-feedback backend by default (spec 5.2); --cpu selects the CPU
    // backend for A/B comparison. Capacity must satisfy the ring invariant
    // C >= spawn_rate * max_lifetime (2000/s * 12s = 24000) so the emit head only
    // laps onto already-dead slots; 32000 leaves margin.
    ParticleSystem* sys = create_particle_system("spores");
    particle_system_set_backend(sys, g_use_cpu ? create_cpu_particle_sim_backend()
                                               : create_tf_particle_sim_backend());

    ParticleEmitter* em = create_particle_emitter("spore", 32000);
    particle_emitter_set_renderer(em, create_billboard_particle_renderer(particle_prog));
    particle_emitter_add_module(em, particle_module_spawn_rate(2000.0f));
    particle_emitter_add_module(
        em, particle_module_init_box_location((vec3){-8, 0, -8}, (vec3){8, 7, 8}));
    particle_emitter_add_module(em, particle_module_init_lifetime(6.0f, 12.0f));
    particle_emitter_add_module(em, particle_module_init_size(0.02f, 0.07f));
    // Pale, faintly-green cordyceps tone (HDR base; the shader's hdrGain pushes
    // it over 1.0 so bloom haloes the motes).
    particle_emitter_add_module(
        em, particle_module_init_color((vec4){0.50f, 0.60f, 0.36f, 1.0f}, 0.07f));
    particle_emitter_add_module(em, particle_module_update_curl_noise(0.25f, 0.5f, 0.15f));
    particle_emitter_add_module(em, particle_module_update_drift((vec3){0.0f, 0.02f, 0.0f}));
    particle_emitter_add_module(em, particle_module_update_integrate(0.985f));
    particle_system_add_emitter(sys, em);
    add_particle_system_to_scene(scene, sys); // scene owns it (ticked + drawn automatically)

    SceneNode* spore_node = create_node();
    set_node_name(spore_node, "spore_emitter");
    set_node_particle_system(spore_node, sys); // node transform = emitter spawn frame
    add_child_node(root, spore_node);
}

static void on_render(Game* game, double alpha) {
    (void)alpha;
    Engine* engine = game->engine;
    Scene* scene = game->scene;
    if (!scene || !scene->root_node)
        return;

    if (g_drag && app_can_process_3d_input(engine)) {
        mouse_drag_update(g_drag, glfwGetTime());
    }

    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(scene->root_node, engine->model_matrix);

    // The scene's particle systems are ticked + drawn by the engine; nothing to
    // do here but render the scene.
    render_current_scene(engine, game->time);
}

static void on_shutdown(Game* game) {
    (void)game;
    // The particle system is owned by the scene (freed in free_scene).
    if (g_drag) {
        free_mouse_drag_controller(g_drag);
        g_drag = NULL;
    }
}

static void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (g_drag) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(g_drag, button, action, mods, x, y);
    }
}

int main(int argc, char** argv) {
    GameConfig config = game_default_config();
    config.title = "Cetra Spores";
    config.width = 1280;
    config.height = 720;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 || strcmp(argv[i], "-x") == 0) {
            config.headless = true;
        } else if ((strcmp(argv[i], "--frames") == 0 || strcmp(argv[i], "-f") == 0) &&
                   i + 1 < argc) {
            config.exit_after_frames = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--screenshot") == 0 || strcmp(argv[i], "-S") == 0) &&
                   i + 1 < argc) {
            config.screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--cpu") == 0) {
            g_use_cpu = true;
        }
    }

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "Failed to create game\n");
        return 1;
    }

    set_engine_mouse_button_callback(game->engine, mouse_button_callback);
    game_set_init(game, on_init);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    run_game(game);
    free_game(game);
    return 0;
}
