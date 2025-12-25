// Game Test - Demonstrates the CharacterController integration
//
// Press WASD to move the player
// Press Space to jump (only when grounded)
// Press F to spawn a falling box
// Press P to pause/unpause
// Press G to print ground state
// Press R to raycast downward
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
#include "cetra/game/character.h"
#include "cetra/ibl.h"

static MouseDragController* drag_controller = NULL;
static Entity* player_entity = NULL;
static Entity* door_entity = NULL;
static Constraint* door_hinge = NULL;
static ShaderProgram* pbr_shader = NULL;
static int box_count = 0;
static const char* hdr_path = NULL;

// Deferred door action (set in callback, applied in update)
static bool door_open_pending = false;
static float door_open_velocity = 0.0f;

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

// Create a door with hinge constraint
static void create_door(Game* game, vec3 position) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);

    if (!em || !physics || !scene)
        return;

    // Door dimensions
    float door_width = 4.0f;
    float door_height = 6.0f;
    float door_thickness = 0.3f;

    // Create door frame (static anchor point)
    Entity* frame = create_entity(em, "door_frame");
    vec3 frame_pos;
    glm_vec3_copy(position, frame_pos);
    frame_pos[1] = door_height / 2.0f; // Center vertically
    entity_set_position(frame, frame_pos);

    // Frame visual (post at hinge edge)
    vec3 frame_size = {0.2f, door_height / 2.0f, 0.2f};
    vec3 frame_color = {0.4f, 0.3f, 0.2f};
    SceneNode* frame_node = create_box_node(scene, frame_size, frame_color, false);
    set_node_name(frame_node, "door_frame");
    frame->node = frame_node;

    // Frame physics (static)
    PhysicsShapeDesc frame_shape = {
        .type = SHAPE_BOX, .box.half_extents = {0.2f, door_height / 2.0f, 0.2f}, .density = 0.0f};
    RigidBody* frame_body =
        entity_add_rigid_body(frame, physics, &frame_shape, MOTION_STATIC, OBJ_LAYER_STATIC);

    // Create door panel (dynamic)
    door_entity = create_entity(em, "door");
    vec3 door_pos;
    glm_vec3_copy(position, door_pos);
    // Position door so its left edge aligns with frame's right edge (no overlap)
    float frame_half_width = 0.2f;
    door_pos[0] += frame_half_width + door_width / 2.0f;
    door_pos[1] = door_height / 2.0f;
    entity_set_position(door_entity, door_pos);

    // Door visual
    vec3 door_size = {door_width / 2.0f, door_height / 2.0f, door_thickness / 2.0f};
    vec3 door_color = {0.6f, 0.4f, 0.2f}; // Wood brown
    SceneNode* door_node = create_box_node(scene, door_size, door_color, false);
    set_node_name(door_node, "door");
    door_entity->node = door_node;

    // Door physics (dynamic) - wooden door ~20-30kg
    // Volume = 4m * 6m * 0.3m = 7.2m³, density = 4 gives ~29kg
    PhysicsShapeDesc door_shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {door_width / 2.0f, door_height / 2.0f, door_thickness / 2.0f},
        .density = 4.0f};
    RigidBody* door_body =
        entity_add_rigid_body(door_entity, physics, &door_shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);

    // Create hinge constraint
    // Both anchors meet at frame's right edge = door's left edge
    ConstraintDesc hinge_desc = {.type = CONSTRAINT_HINGE,
                                 .anchor_a = {frame_half_width, 0, 0},   // Right edge of frame
                                 .anchor_b = {-door_width / 2.0f, 0, 0}, // Left edge of door
                                 .num_velocity_steps = 10,               // Moderate rigidity
                                 .num_position_steps = 4,
                                 .hinge = {
                                     .axis = {0, 1, 0},           // Vertical hinge axis
                                     .min_angle = -GLM_PI * 0.6f, // Allow swing when pushed
                                     .max_angle = GLM_PI * 0.6f,
                                     .max_friction_torque = 0.5f // Low friction for easy swing
                                 }};

    door_hinge = create_constraint(physics, frame_body, door_body, &hinge_desc);
    if (door_hinge) {
        physics_world_add_constraint(physics, door_hinge);
        printf("Door created with hinge constraint at (%.1f, %.1f, %.1f)\n", position[0],
               position[1], position[2]);
    }
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

    // Add physics body (density ~50 kg/m³, like a light wooden crate)
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {half_extents[0], half_extents[1], half_extents[2]},
        .density = 50.0f};
    entity_add_rigid_body(box, physics, &shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);

    printf("Spawned %s at (%.1f, %.1f, %.1f)\n", name, pos[0], pos[1], pos[2]);
}

// Track if player is touching door this frame (declared before on_update uses it)
static bool player_touching_door = false;

// Character contact callback for door interaction
static void on_player_contact(CharacterController* cc, Entity* hit_entity, vec3 contact_position,
                              vec3 contact_normal, void* user_data) {
    (void)contact_position;
    (void)user_data;

    // Check if we hit the door
    if (hit_entity == door_entity && door_hinge) {
        player_touching_door = true;

        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);

        // Get player velocity to determine push direction
        vec3 player_vel;
        character_controller_get_velocity(cc, player_vel);

        // Only push door if player is moving
        if (glm_vec3_norm(player_vel) > 0.1f) {
            // Determine push direction based on velocity and position relative to door
            vec3 to_door;
            glm_vec3_sub(door_entity->position, player_entity->position, to_door);
            float cross_y = to_door[0] * player_vel[2] - to_door[2] * player_vel[0];

            // Defer motor change to update (can't call from callback - threading)
            door_open_pending = true;
            door_open_velocity = (cross_y > 0) ? 6.0f : -6.0f;
        }
    }
}

// Collision callback
static void on_collision(const CollisionEvent* event, void* user_data) {
    (void)user_data;

    // Only handle BEGIN events for gameplay
    if (event->type != COLLISION_BEGIN)
        return;

    const char* name_a = event->entity_a ? event->entity_a->name : "?";
    const char* name_b = event->entity_b ? event->entity_b->name : "?";

    // Check if player hit the door
    bool player_hit_door = (event->entity_a == player_entity && event->entity_b == door_entity) ||
                           (event->entity_b == player_entity && event->entity_a == door_entity);

    if (player_hit_door) {
        // Don't apply explicit impulse - kinematic player contact forces push door naturally
        // This is gentler and less likely to overwhelm the constraint solver
        printf("Player touching door\n");
    }

    // Log player collisions
    bool player_involved = (event->entity_a == player_entity || event->entity_b == player_entity);
    if (player_involved) {
        printf("Player collision: %s <-> %s\n", name_a, name_b);
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

    // Create player entity with CharacterController
    player_entity = create_entity(em, "player");
    entity_set_position(player_entity, (vec3){0, 2.0f, 0});

    // Player visual (capsule approximated as box for now)
    vec3 player_size = {0.5f, 1.0f, 0.5f};
    vec3 player_color = {0.8f, 0.2f, 0.2f};
    SceneNode* player_node = create_box_node(scene, player_size, player_color, false);
    set_node_name(player_node, "player");
    player_entity->node = player_node;

    // Player character controller
    CharacterControllerConfig player_config = character_controller_default_config();
    player_config.capsule_radius = 0.5f;
    player_config.capsule_half_height = 0.5f;
    player_config.step_height = 0.4f;
    player_config.max_strength = 200.0f; // Strong enough to push door

    CharacterController* cc =
        entity_add_character_controller(player_entity, physics, &player_config);
    if (cc) {
        character_controller_set_contact_callback(cc, on_player_contact, game);
    }
    printf("Player created with CharacterController\n");

    // Create a door with hinge constraint
    create_door(game, (vec3){5.0f, 0.0f, 0.0f});

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
    CharacterController* cc = entity_get_character_controller(player_entity);
    if (!cc)
        return;

    // Handle deferred door opening (from contact callback)
    if (door_open_pending && door_hinge) {
        constraint_hinge_set_motor_state(door_hinge, MOTOR_VELOCITY);
        constraint_hinge_set_target_velocity(door_hinge, door_open_velocity);
        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);
        door_open_pending = false;
    }
    // Close door if player wasn't touching it last frame
    else if (!player_touching_door && door_hinge) {
        // Use velocity mode for constant closing speed
        float current_angle = constraint_hinge_get_current_angle(door_hinge);
        if (fabsf(current_angle) > 0.05f) {
            // Door is open - close at constant velocity
            float close_velocity = (current_angle > 0) ? -3.0f : 3.0f;
            constraint_hinge_set_motor_state(door_hinge, MOTOR_VELOCITY);
            constraint_hinge_set_target_velocity(door_hinge, close_velocity);
        } else {
            // Door is nearly closed - switch to position to hold at 0
            constraint_hinge_set_motor_state(door_hinge, MOTOR_POSITION);
            constraint_hinge_set_target_angle(door_hinge, 0.0f);
        }
        // Wake up the door so motor can move it
        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);
    }
    // Reset for this frame - contact callbacks will set it if touching
    player_touching_door = false;

    // Get movement input
    vec3 input_dir;
    input_wasd_direction(&game->input, input_dir);

    // Get current velocity
    vec3 vel;
    character_controller_get_velocity(cc, vel);

    // Apply horizontal movement
    float speed = 10.0f;
    vel[0] = input_dir[0] * speed;
    vel[2] = input_dir[2] * speed;

    // Apply gravity
    float gravity = 20.0f;
    vel[1] -= gravity * (float)dt;

    // Jump when on ground
    if (input_key_pressed(&game->input, GLFW_KEY_SPACE) && character_controller_is_grounded(cc)) {
        vel[1] = 10.0f; // Jump velocity
        printf("Jump!\n");
    }

    // Set velocity (CharacterController will handle collision response)
    character_controller_set_velocity(cc, vel);

    // Door closing is now handled at START of next frame, after we know contact state
    // See beginning of on_update

    // Spawn box on F key
    if (input_key_pressed(&game->input, GLFW_KEY_F)) {
        spawn_falling_box(game);
    }

    // Toggle pause
    if (input_key_pressed(&game->input, GLFW_KEY_P)) {
        game_toggle_pause(game);
        printf("Game %s\n", game_is_paused(game) ? "PAUSED" : "RESUMED");
    }

    // Raycast test on R key - cast ray downward from player
    if (input_key_pressed(&game->input, GLFW_KEY_R) && physics) {
        vec3 down = {0, -1, 0};
        RaycastHit hit;
        if (physics_world_raycast(physics, player_entity->position, down, 50.0f, &hit)) {
            printf("Raycast hit: %s at distance %.2f (pos: %.1f, %.1f, %.1f)\n",
                   hit.entity ? hit.entity->name : "unknown", hit.distance, hit.position[0],
                   hit.position[1], hit.position[2]);
        } else {
            printf("Raycast: no hit\n");
        }
    }

    // Print ground state on G key
    if (input_key_pressed(&game->input, GLFW_KEY_G)) {
        CharacterGroundState state = character_controller_get_ground_state(cc);
        const char* state_str = "unknown";
        switch (state) {
            case CHAR_GROUND_ON_GROUND:
                state_str = "ON_GROUND";
                break;
            case CHAR_GROUND_ON_STEEP_GROUND:
                state_str = "ON_STEEP_GROUND";
                break;
            case CHAR_GROUND_NOT_SUPPORTED:
                state_str = "NOT_SUPPORTED";
                break;
            case CHAR_GROUND_IN_AIR:
                state_str = "IN_AIR";
                break;
        }
        printf("Ground state: %s\n", state_str);
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
    printf("  Escape - Quit\n");
    printf("\nWalk into the door (right side) to push it open!\n\n");

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
