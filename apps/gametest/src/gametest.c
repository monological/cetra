// Game Test - Demonstrates the physics integration
//
// Press WASD to move the player cube
// Press Space to jump
// Press F to spawn a falling box
// Press P to pause/unpause
// Press Escape to quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"
#include "cetra/ibl.h"

static MouseDragController* drag_controller = NULL;
static Entity* player_entity = NULL;
static ShaderProgram* pbr_shader = NULL;
static int box_count = 0;
static const char* hdr_path = NULL;

// Create a visual mesh node for an entity
static SceneNode* create_box_node(Scene* scene, vec3 size, vec3 color, bool glass) {
    SceneNode* node = create_node();

    Mesh* mesh = create_mesh();
    Box box = {.position = {0, 0, 0}, .size = {size[0] * 2, size[1] * 2, size[2] * 2}};
    generate_box_to_mesh(mesh, &box);

    Material* mat = create_material();
    glm_vec3_copy(color, mat->albedo);
    if (glass) {
        mat->roughness = 0.05f;
        mat->metallic = 0.0f;
        mat->opacity = 0.2f;
        mat->ior = 1.5f;
    } else {
        mat->roughness = 0.4f;
        mat->metallic = 0.3f;
    }
    set_material_shader_program(mat, pbr_shader);
    mesh->material = mat;

    add_mesh_to_node(node, mesh);
    add_child_node(scene->root_node, node);
    upload_mesh_buffers_to_gpu(mesh);

    return node;
}

// Spawn a falling box at a random position above the scene
static void spawn_falling_box(Game* game) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);

    if (!em || !physics || !scene)
        return;

    char name[32];
    snprintf(name, sizeof(name), "box_%d", box_count++);

    Entity* box = create_entity(em, name);

    // Random position above the scene
    float x = ((float)rand() / RAND_MAX - 0.5f) * 20.0f;
    float z = ((float)rand() / RAND_MAX - 0.5f) * 20.0f;
    vec3 pos = {x, 15.0f + (float)rand() / RAND_MAX * 5.0f, z};
    entity_set_position(box, pos);

    // Random color
    vec3 color = {0.3f + (float)rand() / RAND_MAX * 0.7f, 0.3f + (float)rand() / RAND_MAX * 0.7f,
                  0.3f + (float)rand() / RAND_MAX * 0.7f};

    // Random size
    float scale = 0.5f + (float)rand() / RAND_MAX * 1.0f;
    vec3 half_extents = {scale, scale, scale};

    // Create visual
    SceneNode* node = create_box_node(scene, half_extents, color, false);
    set_node_name(node, name);
    box->node = node;

    // Add physics body
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {half_extents[0], half_extents[1], half_extents[2]},
        .density = 1000.0f};
    entity_add_rigid_body(box, physics, &shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);

    printf("Spawned %s at (%.1f, %.1f, %.1f)\n", name, pos[0], pos[1], pos[2]);
}

// Collision callback
static void on_collision(const CollisionEvent* event, void* user_data) {
    (void)user_data;

    // Only print BEGIN and END events (STAY would be too noisy)
    if (event->type == COLLISION_STAY)
        return;

    const char* type_str = event->type == COLLISION_BEGIN ? "BEGIN" : "END";
    const char* name_a = event->entity_a ? event->entity_a->name : "?";
    const char* name_b = event->entity_b ? event->entity_b->name : "?";

    // Check if player is involved
    bool player_involved = (event->entity_a && strcmp(event->entity_a->name, "player") == 0) ||
                           (event->entity_b && strcmp(event->entity_b->name, "player") == 0);

    if (player_involved && event->type == COLLISION_BEGIN) {
        printf("Player collision %s: %s <-> %s at (%.1f, %.1f, %.1f)\n", type_str, name_a, name_b,
               event->contact_point[0], event->contact_point[1], event->contact_point[2]);
    }
}

// Game init callback
static void on_init(Game* game) {
    printf("Game initialized with physics!\n");

    Engine* engine = game->engine;

    // Get shaders
    pbr_shader = get_engine_shader_program_by_name(engine, "pbr");
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

    // Load IBL environment if HDR path provided
    if (hdr_path) {
        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_exposure = 1.0f;
                printf("Loaded HDR environment: %s\n", hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
        }
    }

    // Add lights
    create_three_point_lights(scene, 1.0f);

    // Create physics world
    PhysicsConfig physics_config = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&physics_config);
    if (!physics) {
        fprintf(stderr, "Failed to create physics world!\n");
        return;
    }
    game_set_physics_world(game, physics);
    printf("Physics world created\n");

    // Set up collision callback
    physics_world_set_collision_callback(physics, on_collision, game);

    // Create entity manager
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    // Create floor entity (static physics body)
    Entity* floor = create_entity(em, "floor");
    entity_set_position(floor, (vec3){0, -0.5f, 0});

    // Floor visual
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
    set_material_shader_program(floor_mat, pbr_shader);
    floor_mesh->material = floor_mat;

    add_mesh_to_node(floor_node, floor_mesh);
    add_child_node(root, floor_node);
    floor->node = floor_node;

    // Floor physics (static box)
    PhysicsShapeDesc floor_shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {25.0f, 0.5f, 25.0f},
        .density = 0.0f // Static body
    };
    entity_add_rigid_body(floor, physics, &floor_shape, MOTION_STATIC, OBJ_LAYER_STATIC);
    printf("Floor created with static physics\n");

    // Create player entity (kinematic - we control it)
    player_entity = create_entity(em, "player");
    entity_set_position(player_entity, (vec3){0, 2.0f, 0});

    // Player visual
    vec3 player_size = {1.0f, 1.0f, 1.0f};
    vec3 player_color = {0.8f, 0.2f, 0.2f};
    SceneNode* player_node = create_box_node(scene, player_size, player_color, false);
    set_node_name(player_node, "player");
    player_entity->node = player_node;

    // Player physics (kinematic)
    PhysicsShapeDesc player_shape = {
        .type = SHAPE_BOX, .box.half_extents = {1.0f, 1.0f, 1.0f}, .density = 1000.0f};
    entity_add_rigid_body(player_entity, physics, &player_shape, MOTION_KINEMATIC,
                          OBJ_LAYER_KINEMATIC);
    printf("Player created with kinematic physics\n");

    // Upload all GPU buffers
    upload_buffers_to_gpu_for_nodes(root);

    // Optimize broad phase after adding initial bodies
    physics_world_optimize(physics);

    // Setup camera
    Camera* camera = create_camera();
    vec3 cam_pos = {0.0f, 20.0f, 35.0f};
    vec3 look_at = {0.0f, 0.0f, 0.0f};
    vec3 up = {0.0f, 1.0f, 0.0f};
    set_camera_position(camera, cam_pos);
    set_camera_look_at(camera, look_at);
    set_camera_up_vector(camera, up);
    set_camera_perspective(camera, 0.8f, 0.1f, 1000.0f);
    set_engine_camera(engine, camera);
    set_engine_camera_mode(engine, CAMERA_MODE_ORBIT);
    camera->distance = 40.0f;

    // Create drag controller
    drag_controller = create_mouse_drag_controller(engine);

    set_engine_show_gui(engine, true);
    set_engine_show_fps(engine, true);
    set_engine_show_xyz(engine, true);

    // Spawn a few initial boxes
    for (int i = 0; i < 5; i++) {
        spawn_falling_box(game);
    }
}

// Fixed timestep update - game logic and physics
static void on_update(Game* game, double dt) {
    if (!player_entity)
        return;

    PhysicsWorld* physics = game_get_physics_world(game);

    // Get movement input
    vec3 input_dir;
    input_wasd_direction(&game->input, input_dir);

    // Apply movement to player position
    float speed = 10.0f;
    vec3 movement;
    glm_vec3_scale(input_dir, (float)(speed * dt), movement);

    vec3 new_pos;
    glm_vec3_copy(player_entity->position, new_pos);
    glm_vec3_add(new_pos, movement, new_pos);

    // Keep above floor
    if (new_pos[1] < 2.0f) {
        new_pos[1] = 2.0f;
    }

    // Update entity position (will be synced to physics as kinematic)
    entity_set_position(player_entity, new_pos);

    // Spawn box on F key
    if (input_key_pressed(&game->input, GLFW_KEY_F)) {
        spawn_falling_box(game);
    }

    // Toggle pause
    if (input_key_pressed(&game->input, GLFW_KEY_P)) {
        game_toggle_pause(game);
        printf("Game %s\n", game_is_paused(game) ? "PAUSED" : "RESUMED");
    }

    // Apply upward impulse with Space (jump-like effect on nearby dynamic bodies)
    if (input_key_pressed(&game->input, GLFW_KEY_SPACE) && physics) {
        EntityManager* em = game_get_entity_manager(game);
        if (em) {
            // Push all dynamic bodies up
            for (size_t i = 0; i < em->count; i++) {
                Entity* e = em->entities[i];
                if (e == player_entity)
                    continue;
                RigidBody* body = entity_get_rigid_body(e);
                if (body && body->motion_type == MOTION_DYNAMIC) {
                    rigid_body_add_impulse(body, (vec3){0, 500.0f, 0});
                }
            }
            printf("Applied upward impulse to all boxes!\n");
        }
    }

    // Raycast test on R key - cast ray downward from player
    if (input_key_pressed(&game->input, GLFW_KEY_R) && physics) {
        vec3 down = {0, -1, 0};
        RaycastHit hit;
        RigidBody* player_rb = entity_get_rigid_body(player_entity);
        if (physics_world_raycast_ignore(physics, player_entity->position, down, 50.0f, player_rb,
                                         &hit)) {
            printf("Raycast hit: %s at distance %.2f (pos: %.1f, %.1f, %.1f)\n",
                   hit.entity ? hit.entity->name : "unknown", hit.distance, hit.position[0],
                   hit.position[1], hit.position[2]);
        } else {
            printf("Raycast: no hit\n");
        }
    }
}

// Render callback - runs every frame
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

    // Disable backface culling for glass transparency
    glDisable(GL_CULL_FACE);

    // Render the scene
    render_current_scene(engine, game->time);

    // Re-enable culling
    glEnable(GL_CULL_FACE);
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

int main(int argc, const char* argv[]) {
    printf("=== Physics Test ===\n\n");

    // Parse command line arguments
    if (argc > 1) {
        hdr_path = argv[1];
        printf("Using HDR environment: %s\n\n", hdr_path);
    }

    printf("Controls:\n");
    printf("  WASD - Move player cube\n");
    printf("  F - Spawn falling box\n");
    printf("  Space - Push all boxes up\n");
    printf("  R - Raycast downward from player\n");
    printf("  P - Pause/unpause physics\n");
    printf("  Mouse drag - Orbit camera\n");
    printf("  Escape - Quit\n\n");

    srand(42); // Deterministic random for testing

    // Create game
    GameConfig config = game_default_config();
    config.title = "Physics Test - JoltC Integration";
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
