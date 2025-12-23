// Game Test - Demonstrates the game module with WASD movement
//
// Press WASD to move the cube
// Press Space to move up, Shift to move down
// Press P to pause/unpause
// Press Escape to quit

#include <stdio.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/scene.h"
#include "cetra/engine.h"
#include "cetra/render.h"
#include "cetra/geometry.h"
#include "cetra/light.h"
#include "cetra/app.h"
#include "cetra/transform.h"
#include "cetra/game/game.h"

// Player state
typedef struct {
    vec3 position;
    float speed;
    SceneNode* node;
} Player;

static Player player;
static MouseDragController* drag_controller = NULL;

// Game init callback
static void on_init(Game* game) {
    printf("Game initialized!\n");

    Engine* engine = game->engine;

    // Get shaders
    ShaderProgram* pbr = get_engine_shader_program_by_name(engine, "pbr");
    ShaderProgram* xyz = get_engine_shader_program_by_name(engine, "xyz");

    // Create scene
    Scene* scene = create_scene();
    SceneNode* root = create_node();
    set_node_name(root, "root");
    set_scene_root_node(scene, root);
    game_set_scene(game, scene);

    if (xyz) {
        set_scene_xyz_shader_program(scene, xyz);
    }

    // Add lights
    create_three_point_lights(scene, 1.0f);

    // Create floor using Plane primitive
    SceneNode* floor_node = create_node();
    set_node_name(floor_node, "floor");

    Mesh* floor_mesh = create_mesh();
    Plane floor_plane = {
        .position = {0, 0, 0}, .width = 50.0f, .depth = 50.0f, .segments_w = 10, .segments_d = 10};
    generate_plane_to_mesh(floor_mesh, &floor_plane);

    Material* floor_mat = create_material();
    floor_mat->albedo[0] = 0.3f;
    floor_mat->albedo[1] = 0.3f;
    floor_mat->albedo[2] = 0.35f;
    floor_mat->roughness = 0.8f;
    floor_mat->metallic = 0.0f;
    set_material_shader_program(floor_mat, pbr);
    floor_mesh->material = floor_mat;

    add_mesh_to_node(floor_node, floor_mesh);
    add_child_node(root, floor_node);

    // Create player cube using Box primitive
    player.node = create_node();
    set_node_name(player.node, "player");

    Mesh* cube_mesh = create_mesh();
    Box player_box = {.position = {0, 0, 0}, .size = {2.0f, 2.0f, 2.0f}};
    generate_box_to_mesh(cube_mesh, &player_box);

    Material* cube_mat = create_material();
    cube_mat->albedo[0] = 0.8f;
    cube_mat->albedo[1] = 0.2f;
    cube_mat->albedo[2] = 0.2f;
    cube_mat->roughness = 0.4f;
    cube_mat->metallic = 0.3f;
    set_material_shader_program(cube_mat, pbr);
    cube_mesh->material = cube_mat;

    add_mesh_to_node(player.node, cube_mesh);
    add_child_node(root, player.node);

    // Initialize player
    glm_vec3_zero(player.position);
    player.position[1] = 1.0f;
    player.speed = 10.0f;

    // Set initial player transform
    glm_mat4_identity(player.node->original_transform);
    glm_translate(player.node->original_transform, player.position);

    // Upload to GPU
    upload_buffers_to_gpu_for_nodes(root);

    // Setup camera
    Camera* camera = create_camera();
    vec3 cam_pos = {0.0f, 15.0f, 25.0f};
    vec3 look_at = {0.0f, 0.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    set_camera_position(camera, cam_pos);
    set_camera_look_at(camera, look_at);
    set_camera_up_vector(camera, up);
    set_camera_perspective(camera, 0.8f, 0.1f, 1000.0f);
    set_engine_camera(engine, camera);
    set_engine_camera_mode(engine, CAMERA_MODE_ORBIT);
    camera->distance = 30.0f;

    // Create drag controller
    drag_controller = create_mouse_drag_controller(engine);

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_xyz(engine, true);
}

// Fixed timestep update - game logic
static void on_update(Game* game, double dt) {
    // Get movement input
    vec3 input_dir;
    input_wasd_direction(&game->input, input_dir);

    // Vertical movement
    if (input_key_down(&game->input, GLFW_KEY_SPACE)) {
        input_dir[1] = 1.0f;
    }
    if (input_key_down(&game->input, GLFW_KEY_LEFT_SHIFT)) {
        input_dir[1] = -1.0f;
    }

    // Toggle pause
    if (input_key_pressed(&game->input, GLFW_KEY_P)) {
        game_toggle_pause(game);
        printf("Game %s\n", game_is_paused(game) ? "PAUSED" : "RESUMED");
    }

    // Apply movement
    vec3 movement;
    glm_vec3_scale(input_dir, (float)(player.speed * dt), movement);
    glm_vec3_add(player.position, movement, player.position);

    // Keep above floor
    if (player.position[1] < 1.0f) {
        player.position[1] = 1.0f;
    }

    // Update player node transform
    glm_mat4_identity(player.node->original_transform);
    glm_translate(player.node->original_transform, player.position);
}

// Render callback - runs every frame, handles camera and rendering
static void on_render(Game* game, double alpha) {
    (void)alpha;

    Engine* engine = game->engine;
    Scene* scene = game->scene;

    if (!scene || !scene->root_node) {
        return;
    }

    // Update camera with drag controller
    if (drag_controller && app_can_process_3d_input(engine)) {
        mouse_drag_update(drag_controller, glfwGetTime());
    }

    // Apply transforms
    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(scene->root_node, engine->model_matrix);

    // Render the scene
    render_current_scene(engine, game->time);
}

// Shutdown callback
static void on_shutdown(Game* game) {
    (void)game;
    printf("Game shutting down...\n");

    if (drag_controller) {
        free_mouse_drag_controller(drag_controller);
        drag_controller = NULL;
    }
}

// Mouse callback for camera control
static void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (drag_controller) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(drag_controller, button, action, mods, x, y);
    }
}

int main(void) {
    printf("=== Game Test ===\n\n");
    printf("Controls:\n");
    printf("  WASD - Move horizontally\n");
    printf("  Space/Shift - Move up/down\n");
    printf("  P - Pause/unpause\n");
    printf("  Mouse drag - Orbit camera\n");
    printf("  Escape - Quit\n\n");

    // Create game
    GameConfig config = game_default_config();
    config.title = "Game Test - WASD Movement";
    config.width = 1280;
    config.height = 720;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "Failed to create game\n");
        return -1;
    }

    // Set mouse callback
    set_engine_mouse_button_callback(game->engine, mouse_button_callback);

    // Set game callbacks
    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    // Run the game
    run_game(game);

    // Cleanup
    free_game(game);

    printf("Goodbye!\n");
    return 0;
}
