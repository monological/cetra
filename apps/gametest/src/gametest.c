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
#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/scene.h"
#include "cetra/engine.h"
#include "cetra/geometry.h"
#include "cetra/light.h"
#include "cetra/app.h"
#include "cetra/game/game.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"
#include "cetra/game/character.h"
#include "cetra/game/audio.h"
#include "cetra/game/animator_component.h"
#include "cetra/animator.h"
#include "cetra/import.h"
#include "cetra/ibl.h"

static MouseDragController* drag_controller = NULL;
static Entity* player_entity = NULL;
static Entity* door_entity = NULL;
static Constraint* door_hinge = NULL;
static ShaderProgram* pbr_shader = NULL;
static int box_count = 0;
static const char* hdr_path = NULL;

// Animation (spec 12.1): the player is the procedural puppet on an ANIMATOR
// component -- idle, walk and run blended from the controller's post-solve
// speed, a jump one-shot, a wave on the masked override layer, footsteps from
// the clips' events. --no-puppet keeps the red box; --twin <clip> stands a
// second rig beside the player playing its own clip, which is the per-node
// pose seen from a game.
#define PLAYER_SPEED 10.0f
// Capsule centre to feet: radius 0.5 + half-height 0.5. The yaw rebuilds
// this transform every step, so both sites read the same number.
#define PLAYER_RIG_DROP (-1.0f)
static bool no_puppet = false;
static const char* puppet_path = "assets/puppet.gltf";
static const char* twin_clip = NULL;
static SceneNode* player_rig = NULL; // the puppet under the entity's node: drop + yaw
static Animator* player_animator = NULL;
static AnimatorEntry locomotion[3];
static Animation* clip_jump = NULL;
static Animation* clip_wave = NULL;
static float wave_mask[MAX_BONES];
static float player_yaw = 0.0f;
static Sound* step_sound = NULL;

// --trace-player: the player's pose and the input it acted on, printed every
// trace_every fixed steps, which is what the gate group reads.
static bool trace_player = false;
static int trace_every = 30;
static int trace_step = 0;

// Audio (spec 12.0): procedural tones, so the demo ships no audio files. A beep
// on jump and on spawn, and a looping tone carried by the door as an
// AUDIO_SOURCE component -- it pans and fades as the door swings and the camera
// moves. --mute silences the master bus.
static Sound* jump_sound = NULL;
static Sound* spawn_sound = NULL;
static bool audio_muted = false;

// What the game reads, and which key, pad button or pad axis each one is.
// The stick's Y is negated: GLFW reads it down-positive, and the move helper
// takes +y as forward.
static const InputAction actions[] = {
    {"move_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_AXIS(LEFT_X, 1), INPUT_PAD(DPAD_RIGHT, 1),
      INPUT_PAD(DPAD_LEFT, -1)}},
    {"move_y",
     {INPUT_KEY(W, 1), INPUT_KEY(S, -1), INPUT_AXIS(LEFT_Y, -1), INPUT_PAD(DPAD_UP, 1),
      INPUT_PAD(DPAD_DOWN, -1)}},
    {"jump", {INPUT_KEY(SPACE, 1), INPUT_PAD(A, 1)}},
    {"spawn", {INPUT_KEY(F, 1), INPUT_PAD(X, 1)}},
    {"pause", {INPUT_KEY(P, 1), INPUT_PAD(START, 1)}},
    {"raycast", {INPUT_KEY(R, 1), INPUT_PAD(Y, 1)}},
    {"ground", {INPUT_KEY(G, 1), INPUT_PAD(B, 1)}},
    {"wave", {INPUT_KEY(E, 1), INPUT_PAD(LEFT_BUMPER, 1)}},
};
#define ACTION_COUNT (sizeof(actions) / sizeof(actions[0]))

// Deferred door action (set in callback, applied in update)
static bool door_open_pending = false;
static float door_open_velocity = 0.0f;

// Uniform in [0, 1]. The division is in DOUBLE deliberately: RAND_MAX is 0x7fffffff,
// which float cannot represent, so dividing in float rounds the divisor up to 2^31 and
// the quotient is quietly wrong. double holds it exactly, and one rounding on the way
// out is the whole error.
static float rand01(void) {
    return (float)(rand() / (double)RAND_MAX);
}

// Create a visual mesh node for an entity
static SceneNode* create_box_node(Scene* scene, vec3 size, vec3 color, bool glass) {
    SceneNode* node = create_node();

    Mesh* mesh = create_mesh();
    Box box = {.position = {0, 0, 0}, .size = {size[0] * 2, size[1] * 2, size[2] * 2}};
    mesh_generate_box(mesh, &box);

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
    material_set_program(mat, pbr_shader);
    mesh->material = mat;

    node_add_mesh(node, mesh);
    node_add_child(scene->root_node, node);

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
    glm_vec3_copy(frame_pos, frame->position);

    // Frame visual (post at hinge edge)
    vec3 frame_size = {0.2f, door_height / 2.0f, 0.2f};
    vec3 frame_color = {0.4f, 0.3f, 0.2f};
    SceneNode* frame_node = create_box_node(scene, frame_size, frame_color, false);
    node_set_name(frame_node, "door_frame");
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
    glm_vec3_copy(door_pos, door_entity->position);

    // Door visual
    vec3 door_size = {door_width / 2.0f, door_height / 2.0f, door_thickness / 2.0f};
    vec3 door_color = {0.6f, 0.4f, 0.2f}; // Wood brown
    SceneNode* door_node = create_box_node(scene, door_size, door_color, false);
    node_set_name(door_node, "door");
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
    float x = (rand01() - 0.5f) * 20.0f;
    float z = (rand01() - 0.5f) * 20.0f;
    vec3 pos = {x, 15.0f + rand01() * 5.0f, z};
    glm_vec3_copy(pos, box->position);

    // Random color
    vec3 color = {0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f};

    // Random size
    float scale = 0.5f + rand01() * 1.0f;
    vec3 half_extents = {scale, scale, scale};

    // Create visual
    SceneNode* node = create_box_node(scene, half_extents, color, false);
    node_set_name(node, name);
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

// The footstep events the walk and run cycles carry: one per plant, at the
// two extremes of the cos-phased swing.
static void add_footsteps(Animation* clip) {
    if (!clip)
        return;
    animation_add_event(clip, 0.0f, "step_l");
    animation_add_event(clip, clip->duration * 0.5f, "step_r");
}

static void on_anim_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    (void)user;
    if (step_sound && (!strcmp(name, "step_l") || !strcmp(name, "step_r")))
        audio_sound_play(step_sound);
}

// Put `rig` under a holder that becomes the entity's node, and return the node
// the app may pose.
//
// TWO levels, and both are load-bearing. sync_entity_transforms overwrites the
// entity node's local every step, so the drop from capsule centre to feet and
// the facing yaw cannot live there -- they go on the inner node. And the
// importer's scene root IS the model's own node, so a rig found by name can be
// the root itself: re-parenting that under a child of itself is a cycle, and
// every recursive walk in the engine would run until the stack ran out. Its
// children are the rig in that case, so they move instead.
static SceneNode* attach_rig(Scene* scene, Entity* entity, SceneNode* rig, float drop) {
    SceneNode* holder = create_node();
    node_set_name(holder, entity->name);
    SceneNode* inner = create_node();
    node_set_name(inner, "rig");
    node_add_child(holder, inner);
    if (rig == scene->root_node) {
        // Bounded rather than drained: node_add_child refuses a cycle, and a
        // `while (children_count)` on a refusal spins forever.
        for (size_t guard = rig->children_count; guard > 0 && rig->children_count > 0; guard--)
            node_add_child(inner, rig->children[0]);
    } else {
        node_add_child(inner, rig);
    }
    node_add_child(scene->root_node, holder);
    glm_translate_make(inner->original_transform, (vec3){0.0f, drop, 0.0f});
    entity->node = holder;
    return inner;
}

// A second node tree over the puppet's mesh, shared by reference: two nodes on
// one skinned mesh never batch, and each carries its own pose.
static SceneNode* clone_rig(SceneNode* puppet_root) {
    SceneNode* mesh_node = node_find(puppet_root, "puppet_mesh");
    if (!mesh_node || mesh_node->mesh_count == 0)
        return NULL;
    SceneNode* rig = create_node();
    node_set_name(rig, "twin_rig");
    node_add_mesh(rig, mesh_ref(mesh_node->meshes[0]));
    return rig;
}

// Game init callback
static void on_init(Game* game) {
    printf("Game initialized with physics!\n");

    Engine* engine = game->engine;

    // Get shaders
    pbr_shader = engine_get_program(engine, CETRA_PROGRAM_PBR);
    ShaderProgram* xyz = engine_get_program(engine, CETRA_PROGRAM_XYZ);

    // The scene IS the puppet's when there is one: a material belongs to the
    // scene that registered it, so a rig imported into a second scene cannot
    // be re-parented into this one. Without a puppet, or when it fails to
    // load, the scene is built from nothing as it always was.
    Scene* scene = NULL;
    SceneNode* puppet_root = NULL;
    if (!no_puppet) {
        scene = create_scene_from_model_path(puppet_path, NULL, engine->async_loader);
        if (scene) {
            puppet_root = node_find(scene->root_node, "puppet");
            if (!puppet_root || scene->skeleton_count == 0 || scene->animation_count == 0) {
                fprintf(stderr, "gametest: '%s' has no rig with clips; keeping the box\n",
                        puppet_path);
                free_scene(scene);
                scene = NULL;
                puppet_root = NULL;
            }
        } else {
            fprintf(stderr, "gametest: could not load '%s'; keeping the box\n", puppet_path);
        }
    }
    if (!scene)
        scene = create_scene();
    SceneNode* root = scene->root_node;
    game_set_scene(game, scene);

    if (puppet_root) {
        ShaderProgram* pbr_skinned = create_pbr_skinned_program();
        if (pbr_skinned) {
            engine_add_program(engine, pbr_skinned);
            node_set_programs(puppet_root, pbr_shader, pbr_skinned);
        }
    }

    if (xyz) {
        scene_set_xyz_program(scene, xyz);
    }

    // Load IBL environment if HDR path provided
    if (hdr_path) {
        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_brightness = 1.0f;
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
    scene_add_three_point_lights(scene, 1.0f);

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

    // Audio: one device, two SFX beeps. Headless opens no device (offline).
    AudioSystem* audio = create_audio_system(engine->headless);
    if (audio) {
        game_set_audio_system(game, audio);
        if (audio_muted)
            audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
        jump_sound = audio_sound_from_tone(audio, 660.0f, AUDIO_BUS_SFX);
        spawn_sound = audio_sound_from_tone(audio, 180.0f, AUDIO_BUS_SFX);
        step_sound = audio_sound_from_tone(audio, 110.0f, AUDIO_BUS_SFX);
        if (step_sound)
            audio_sound_set_volume(step_sound, 0.4f);
    }

    // Create floor entity (static physics body)
    Entity* floor = create_entity(em, "floor");
    glm_vec3_copy((vec3){0, -0.5f, 0}, floor->position);

    // Floor visual
    SceneNode* floor_node = create_node();
    node_set_name(floor_node, "floor");
    Mesh* floor_mesh = create_mesh();
    Plane floor_plane = {
        .position = {0, 0, 0}, .width = 50.0f, .depth = 50.0f, .segments_w = 10, .segments_d = 10};
    mesh_generate_plane(floor_mesh, &floor_plane);

    Material* floor_mat = create_material();
    floor_mat->albedo[0] = 0.3f;
    floor_mat->albedo[1] = 0.3f;
    floor_mat->albedo[2] = 0.35f;
    floor_mat->roughness = 0.8f;
    floor_mat->metallic = 0.0f;
    material_set_program(floor_mat, pbr_shader);
    floor_mesh->material = floor_mat;

    node_add_mesh(floor_node, floor_mesh);
    node_add_child(root, floor_node);
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
    glm_vec3_copy((vec3){0, 2.0f, 0}, player_entity->position);

    if (puppet_root) {
        // The puppet, its feet a capsule's half-height plus radius below the
        // entity, on the locomotion space; the box's colour and size are the
        // capsule's, which stays the physics body either way.
        player_rig = attach_rig(scene, player_entity, puppet_root, PLAYER_RIG_DROP);
        Skeleton* skeleton = scene->skeletons[0];
        Animation* idle = scene_find_animation(scene, "idle");
        Animation* walk = scene_find_animation(scene, "walk");
        Animation* run = scene_find_animation(scene, "run");
        clip_jump = scene_find_animation(scene, "jump");
        clip_wave = scene_find_animation(scene, "wave");
        add_footsteps(walk);
        add_footsteps(run);
        animator_mask_subtree(skeleton, "cetra_rig:RightArm", wave_mask);
        locomotion[0] = (AnimatorEntry){idle, 0.0f};
        locomotion[1] = (AnimatorEntry){walk, 0.5f};
        locomotion[2] = (AnimatorEntry){run, 1.0f};
        player_animator = create_animator(skeleton);
        if (player_animator && idle && walk && run) {
            animator_play_space(player_animator, "locomotion", locomotion, 3, 0.0f, true);
            animator_set_event_callback(player_animator, on_anim_event, game);
            entity_add_animator(player_entity, player_animator);
        }
        printf("Player is the puppet: %zu bones, %zu clips\n", skeleton->bone_count,
               scene->animation_count);
    } else {
        // Player visual (capsule approximated as box for now)
        vec3 player_size = {0.5f, 1.0f, 0.5f};
        vec3 player_color = {0.8f, 0.2f, 0.2f};
        SceneNode* player_node = create_box_node(scene, player_size, player_color, false);
        node_set_name(player_node, "player");
        player_entity->node = player_node;
    }

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

    // A second rig beside the player on its own clip: two poses in one frame.
    if (puppet_root && twin_clip) {
        Animation* clip = scene_find_animation(scene, twin_clip);
        SceneNode* rig = clip ? clone_rig(puppet_root) : NULL;
        if (!clip) {
            fprintf(stderr, "gametest: --twin names clip '%s', which the puppet lacks\n",
                    twin_clip);
        } else if (rig) {
            Entity* twin = create_entity(em, "twin");
            glm_vec3_copy((vec3){-6.0f, 0.0f, 0.0f}, twin->position);
            attach_rig(scene, twin, rig, 0.0f);
            Animator* a = create_animator(scene->skeletons[0]);
            if (a) {
                animator_play(a, clip, 0.0f, true);
                entity_add_animator(twin, a);
            }
            printf("Twin rig playing '%s'\n", clip->name);
        }
    }

    // Create a door with hinge constraint
    create_door(game, (vec3){5.0f, 0.0f, 0.0f});

    // A looping tone carried by the door as an AUDIO_SOURCE component: its world
    // position is pushed from the entity each frame, so it pans and attenuates
    // as the door swings and as the camera orbits.
    if (audio && door_entity) {
        Sound* beacon = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        if (beacon) {
            audio_sound_set_looping(beacon, true);
            audio_sound_set_volume(beacon, 0.5f);
            entity_add_audio_source(door_entity, beacon);
            audio_sound_play(beacon);
        }
    }

    // Optimize broad phase after adding initial bodies
    physics_world_optimize(physics);

    // Setup camera
    CameraDesc camera_desc = {
        .position = {0.0f, 20.0f, 35.0f}, .fov = 0.8f, .near = 0.1f, .far = 1000.0f};
    Camera* camera = create_camera(&camera_desc);
    engine_set_camera(engine, camera);
    engine->camera_mode = CAMERA_MODE_ORBIT;

    // Create drag controller
    drag_controller = create_mouse_drag_controller(engine);

    // No GUI or FPS overlay headless, as the other apps: the FPS digits are
    // wall clock and land in the screenshot, which is what made two identical
    // runs differ by pixels while the sim beneath them did not.
    engine->show_gui = !engine->headless;
    engine->show_fps = !engine->headless;
    // A rig brings twenty joint nodes, each of which would wear a gizmo.
    engine->show_xyz = puppet_root == NULL;

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

    vec3 input_dir;
    input_action_move(&game->input, "move_x", "move_y", input_dir);

    // Get current velocity
    vec3 vel;
    character_controller_get_velocity(cc, vel);

    // The locomotion knob and the facing come from this velocity -- Jolt's
    // POST-SOLVE one, before the input below overwrites it -- so walking into
    // a wall stops the walk rather than running on the spot.
    float ground_speed = hypotf(vel[0], vel[2]);
    if (player_animator) {
        float knob = ground_speed / PLAYER_SPEED;
        player_animator->param = knob > 1.0f ? 1.0f : knob;
    }
    if (player_rig && ground_speed > 0.1f) {
        // The puppet faces +Z at yaw 0. Smoothed on sim time, so it is the
        // same turn headless and windowed.
        float target = atan2f(vel[0], vel[2]);
        float delta = target - player_yaw;
        while (delta > (float)M_PI)
            delta -= 2.0f * (float)M_PI;
        while (delta < -(float)M_PI)
            delta += 2.0f * (float)M_PI;
        float k = (float)dt * 12.0f;
        player_yaw += delta * (k > 1.0f ? 1.0f : k);
        glm_mat4_identity(player_rig->original_transform);
        glm_translate(player_rig->original_transform, (vec3){0.0f, PLAYER_RIG_DROP, 0.0f});
        glm_rotate_y(player_rig->original_transform, player_yaw, player_rig->original_transform);
    }

    // Apply horizontal movement
    vel[0] = input_dir[0] * PLAYER_SPEED;
    vel[2] = input_dir[2] * PLAYER_SPEED;

    // Apply gravity
    float gravity = 20.0f;
    vel[1] -= gravity * (float)dt;

    // Jump when on ground
    bool grounded = character_controller_is_grounded(cc);
    bool jump = input_action_pressed(&game->input, "jump");
    if (jump && grounded) {
        vel[1] = 10.0f; // Jump velocity
        printf("Jump!\n");
        if (jump_sound)
            audio_sound_play(jump_sound);
        // The tuck is a one-shot: the airtime is one second at this
        // velocity under this gravity, and the clip returns to the
        // locomotion space by itself.
        if (player_animator && clip_jump)
            animator_play_once(player_animator, clip_jump, 0.25f);
    }
    if (player_animator && clip_wave && input_action_pressed(&game->input, "wave"))
        animator_play_layer(player_animator, clip_wave, wave_mask, 0.1f, 0.1f, false);

    // Set velocity (CharacterController will handle collision response)
    character_controller_set_velocity(cc, vel);

    // The position is the one BEFORE this step; move_x and move_y are the
    // action values the step acted on. With a rig the line continues after
    // `jump`: the knob, the locomotion space's three weights (zeros while a
    // one-shot has the base), the crossfade weight, the override weight and
    // the base source's name -- all as of the last rendered frame's tick.
    if (trace_player && trace_step % trace_every == 0) {
        printf("player step %d t=%5.2f pos %8.3f %8.3f %8.3f  vel %6.2f %6.2f %6.2f  "
               "grounded %d  move %5.2f %5.2f jump %d",
               trace_step, game->time, player_entity->position[0], player_entity->position[1],
               player_entity->position[2], vel[0], vel[1], vel[2], grounded ? 1 : 0,
               input_action_value(&game->input, "move_x"),
               input_action_value(&game->input, "move_y"), jump ? 1 : 0);
        if (player_animator) {
            const AnimatorSpace* base = &player_animator->base;
            bool loco = base->count == 3;
            printf(" anim %.3f %.3f %.3f %.3f %.3f %.3f %s", player_animator->param,
                   loco ? base->weights[0] : 0.0f, loco ? base->weights[1] : 0.0f,
                   loco ? base->weights[2] : 0.0f, player_animator->fade_weight,
                   player_animator->layer.weight, animator_source_name(player_animator));
        }
        printf("\n");
    }
    trace_step++;

    // Door closing is now handled at START of next frame, after we know contact state
    // See beginning of on_update

    if (input_action_pressed(&game->input, "spawn")) {
        spawn_falling_box(game);
        if (spawn_sound)
            audio_sound_play(spawn_sound);
    }

    if (input_action_pressed(&game->input, "raycast") && physics) {
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

    if (input_action_pressed(&game->input, "ground")) {
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

// Pre-render callback - the camera the frame's geometry is read against. The
// engine propagates the graph as soon as this returns.
static void on_pre_render(Game* game, double alpha) {
    (void)alpha;

    // Here and not in on_update: the fixed step does not run while paused, so
    // a toggle read there could pause and never unpause.
    if (input_action_pressed(&game->input, "pause")) {
        game_toggle_pause(game);
        printf("Game %s\n", game_is_paused(game) ? "PAUSED" : "RESUMED");
    }

    Engine* engine = game->engine;
    if (drag_controller && app_can_process_3d_input(engine)) {
        mouse_drag_update(drag_controller, glfwGetTime());
    }
}

// Render callback - runs every frame
static void on_render(Game* game, double alpha) {
    (void)alpha;

    Engine* engine = game->engine;
    const Scene* scene = game->scene;

    if (!scene || !scene->root_node) {
        return;
    }

    // Disable backface culling for glass transparency
    glDisable(GL_CULL_FACE);

    // Render the scene
    engine_render_scene(engine, game->scene);

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
    (void)engine;
    if (drag_controller) {
        mouse_drag_on_button(drag_controller, button, action, mods);
    }
}

// --audio-probe: a deterministic offline render that measures the mixed PCM, so
// the `audio` gate can assert onset, panning, distance falloff, bus routing and
// the file-decode path with no device and no committed audio. Headless only.
#define AUDIO_PROBE_WINDOW 9600 // frames per measurement, 0.2s at the offline 48 kHz

static void probe_measure(AudioSystem* audio, float* rms_l, float* rms_r) {
    float buf[1024]; // up to 512 interleaved stereo frames
    double sum_l = 0.0, sum_r = 0.0;
    long total = 0;
    int remaining = AUDIO_PROBE_WINDOW;
    while (remaining > 0) {
        size_t want = remaining < 512 ? (size_t)remaining : 512;
        size_t got = audio_system_read_pcm(audio, buf, want);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            sum_l += (double)buf[i * 2] * buf[i * 2];
            sum_r += (double)buf[i * 2 + 1] * buf[i * 2 + 1];
        }
        total += (long)got;
        remaining -= (int)got;
    }
    *rms_l = total ? sqrtf((float)(sum_l / total)) : 0.0f;
    *rms_r = total ? sqrtf((float)(sum_r / total)) : 0.0f;
}

static int run_audio_probe(Game* game, const char* which, const char* file) {
    AudioSystem* audio = create_audio_system(game->engine->headless);
    if (!audio) {
        fprintf(stderr, "audio-probe: could not create audio system\n");
        return 1;
    }
    game_set_audio_system(game, audio); // owned; freed by free_game

    vec3 origin = {0, 0, 0}, fwd = {0, 0, -1}, up = {0, 1, 0};
    audio_system_update(audio, NULL, origin, fwd, up); // fix the listener at the origin

    float l = 0.0f, r = 0.0f;
    int rc = 0;

    if (!strcmp(which, "onset")) {
        // A centred 2D tone: silent until played, then energetic.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        probe_measure(audio, &l, &r);
        printf("audio onset pre rms %.6f %.6f\n", l, r);
        audio_sound_play(t);
        probe_measure(audio, &l, &r);
        printf("audio onset post rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "pan")) {
        // Same distance either side, so only the pan differs.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_sound_set_position(t, (vec3){10.0f, 0.0f, 0.0f});
        probe_measure(audio, &l, &r);
        printf("audio pan right rms %.6f %.6f\n", l, r);
        audio_sound_set_position(t, (vec3){-10.0f, 0.0f, 0.0f});
        probe_measure(audio, &l, &r);
        printf("audio pan left rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "distance")) {
        // Straight ahead, so panning is even; only the distance differs.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_sound_set_position(t, (vec3){0.0f, 0.0f, -1.0f});
        probe_measure(audio, &l, &r);
        printf("audio distance near rms %.6f %.6f\n", l, r);
        audio_sound_set_position(t, (vec3){0.0f, 0.0f, -20.0f});
        probe_measure(audio, &l, &r);
        printf("audio distance far rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "master")) {
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 1.0f);
        probe_measure(audio, &l, &r);
        printf("audio master on rms %.6f %.6f\n", l, r);
        audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
        probe_measure(audio, &l, &r);
        printf("audio master off rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "decode")) {
        if (!file) {
            fprintf(stderr, "audio-probe decode: needs --audio-file <wav>\n");
            rc = 1;
        } else {
            Sound* s = audio_sound_from_file(audio, file, AUDIO_BUS_SFX);
            if (!s) {
                fprintf(stderr, "audio-probe decode: could not load %s\n", file);
                rc = 1;
            } else {
                audio_sound_play(s);
                probe_measure(audio, &l, &r);
                printf("audio decode result rms %.6f %.6f\n", l, r);
            }
        }
    } else {
        fprintf(stderr, "audio-probe: unknown case '%s'\n", which);
        rc = 1;
    }
    return rc;
}

// --anim-probe: the animator layer as a game sees it -- ANIMATOR components on
// entities, ticked by the loop's own update_all_animators at the fixed 1/60 --
// measured on the CPU and printed as `anim <case> <label> <key> <numbers>`,
// which is what the `anim` gate group reads. No window, no pixels: the
// puppet's clips are authored in closed form, so every number here has an
// expected value the gate states.
#define PROBE_DT (1.0f / 60.0f)

// Largest element-wise difference between two states' skinning matrices, over
// one bone or (bone < 0) all of them.
static float pose_maxdiff(const AnimationState* a, const AnimationState* b, int bone) {
    float worst = 0.0f;
    size_t lo = bone < 0 ? 0 : (size_t)bone;
    size_t hi = bone < 0 ? a->active_bone_count : (size_t)bone + 1;
    for (size_t i = lo; i < hi; i++) {
        const float* ma = (const float*)a->bone_matrices[i];
        const float* mb = (const float*)b->bone_matrices[i];
        for (int k = 0; k < 16; k++) {
            float d = fabsf(ma[k] - mb[k]);
            if (d > worst)
                worst = d;
        }
    }
    return worst;
}

static Animator* probe_rig(EntityManager* em, Skeleton* skeleton, const char* name) {
    Entity* e = create_entity(em, name);
    Animator* a = create_animator(skeleton);
    if (a)
        entity_add_animator(e, a);
    return a;
}

static void probe_tick(EntityManager* em, int ticks) {
    for (int i = 0; i < ticks; i++)
        update_all_animators(em, PROBE_DT);
}

// The events case's recorder: names, and the tick each fired on.
static int probe_event_count = 0;
static int probe_tick_index = 0;
static void probe_on_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    printf("anim events %s fired %s %d\n", (const char*)user, name, probe_tick_index);
    probe_event_count++;
}

static int run_anim_probe(Game* game, const char* which) {
    Scene* scene = create_scene_from_model_path(puppet_path, NULL, game->engine->async_loader);
    if (!scene || scene->skeleton_count == 0) {
        fprintf(stderr, "anim-probe: could not load a rig from '%s'\n", puppet_path);
        if (scene)
            free_scene(scene);
        return 1;
    }
    game_set_scene(game, scene);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);
    Skeleton* skel = scene->skeletons[0];
    Animation* idle = scene_find_animation(scene, "idle");
    Animation* walk = scene_find_animation(scene, "walk");
    Animation* run = scene_find_animation(scene, "run");
    Animation* wave = scene_find_animation(scene, "wave");
    if (!idle || !walk || !run || !wave) {
        fprintf(stderr, "anim-probe: the rig lacks one of idle/walk/run/wave\n");
        return 1;
    }
    AnimatorEntry loco[3] = {{idle, 0.0f}, {walk, 0.5f}, {run, 1.0f}};
    int rc = 0;

    if (!strcmp(which, "locomotion")) {
        // The three weights at seven knob positions, two of them past the ends.
        Animator* a = probe_rig(em, skel, "a");
        animator_play_space(a, "locomotion", loco, 3, 0.0f, true);
        const float knobs[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, -0.5f};
        const char* labels[] = {"p000", "p025", "p050", "p075", "p100", "p150", "pm050"};
        for (int i = 0; i < 7; i++) {
            a->param = knobs[i];
            probe_tick(em, 1);
            printf("anim locomotion %s weights %.6f %.6f %.6f\n", labels[i], a->base.weights[0],
                   a->base.weights[1], a->base.weights[2]);
        }
    } else if (!strcmp(which, "crossfade")) {
        // A fades idle -> walk over 0.5 s; B plays walk from the switch tick
        // with no fade. After the fade the two must be the same bits.
        Animator* a = probe_rig(em, skel, "a");
        animator_play(a, idle, 0.0f, true);
        probe_tick(em, 10);
        animator_play(a, walk, 0.5f, true);
        Animator* b = probe_rig(em, skel, "b");
        animator_play(b, walk, 0.0f, true);
        printf("anim crossfade t0 fade %.6f\n", a->fade_weight);
        probe_tick(em, 15);
        printf("anim crossfade thalf fade %.6f\n", a->fade_weight);
        probe_tick(em, 15);
        printf("anim crossfade tF fade %.6f\n", a->fade_weight);
        probe_tick(em, 1);
        printf("anim crossfade tpost fade %.6f\n", a->fade_weight);
        printf("anim crossfade tpost settled %d\n", a->fading ? 0 : 1);
        printf("anim crossfade pose maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
    } else if (!strcmp(which, "layer")) {
        // A waves over idle on the right arm's subtree; B is idle alone.
        Animator* a = probe_rig(em, skel, "a");
        Animator* b = probe_rig(em, skel, "b");
        animator_play(a, idle, 0.0f, true);
        animator_play(b, idle, 0.0f, true);
        float mask[MAX_BONES];
        animator_mask_subtree(skel, "cetra_rig:RightArm", mask);
        animator_play_layer(a, wave, mask, 0.1f, 0.1f, false);
        probe_tick(em, 15);
        printf("anim layer w15 weight %.6f\n", a->layer.weight);
        printf("anim layer w15 finished %d\n", animator_layer_finished(a) ? 1 : 0);
        const char* outside[] = {"cetra_rig:Hips", "cetra_rig:Spine2", "cetra_rig:LeftArm"};
        float out_max = 0.0f;
        for (int i = 0; i < 3; i++) {
            float d = pose_maxdiff(a->state, b->state, get_bone_index_by_name(skel, outside[i]));
            out_max = d > out_max ? d : out_max;
        }
        printf("anim layer masked_out maxdiff %.6f\n", out_max);
        printf(
            "anim layer masked_in maxdiff %.6f\n",
            pose_maxdiff(a->state, b->state, get_bone_index_by_name(skel, "cetra_rig:RightArm")));
        probe_tick(em, 78);
        printf("anim layer after weight %.6f\n", a->layer.weight);
        printf("anim layer after finished %d\n", animator_layer_finished(a) ? 1 : 0);
        printf("anim layer after maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
    } else if (!strcmp(which, "two-rigs")) {
        // Three components ticked together: A idle, B run, C run. B and C
        // must agree to the bit, and A must differ from them.
        Animator* a = probe_rig(em, skel, "a");
        Animator* b = probe_rig(em, skel, "b");
        Animator* c = probe_rig(em, skel, "c");
        animator_play(a, idle, 0.0f, true);
        animator_play(b, run, 0.0f, true);
        animator_play(c, run, 0.0f, true);
        probe_tick(em, 30);
        printf("anim two-rigs ab maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
        printf("anim two-rigs bc maxdiff %.6f\n", pose_maxdiff(b->state, c->state, -1));
    } else if (!strcmp(which, "phase")) {
        // Walk and run at the knob's midpoint share one phase, advancing at
        // the weighted mean of their cycle frequencies: 0.5 * 1 Hz + 0.5 *
        // 2 Hz, so 20 ticks is half a cycle of each.
        Animator* a = probe_rig(em, skel, "a");
        AnimatorEntry pair[2] = {{walk, 0.0f}, {run, 1.0f}};
        animator_play_space(a, "pair", pair, 2, 0.0f, true);
        a->param = 0.5f;
        probe_tick(em, 20);
        const Animation* ref = a->base.entries[a->base.ref].clip;
        float phase = a->base.time / ref->duration;
        printf("anim phase walk frac %.6f\n", phase);
        printf("anim phase run frac %.6f\n", phase);
        float hz = 0.5f * (walk->ticks_per_second / walk->duration) +
                   0.5f * (run->ticks_per_second / run->duration);
        printf("anim phase expected frac %.6f\n", 20.0f * PROBE_DT * hz);
    } else if (!strcmp(which, "events")) {
        // Footsteps over 110 ticks (1.83 s): a walk plants four, a run
        // eight, the midpoint mix six from the walk alone (1.5 cycles per
        // second and only the heavier entry fires), and idle none.
        add_footsteps(walk);
        add_footsteps(run);
        Animator* a = probe_rig(em, skel, "a");
        const float knobs[] = {0.5f, 0.75f, 1.0f, 0.0f};
        const char* labels[] = {"walk", "mixed", "run", "idle"};
        for (int i = 0; i < 4; i++) {
            AnimatorEntry pair[2] = {{walk, 0.5f}, {run, 1.0f}};
            if (i == 3)
                animator_play(a, idle, 0.0f, true);
            else
                animator_play_space(a, "pair", pair, 2, 0.0f, true);
            a->param = knobs[i];
            animator_set_event_callback(a, probe_on_event, (void*)labels[i]);
            probe_event_count = 0;
            for (probe_tick_index = 0; probe_tick_index < 110; probe_tick_index++)
                update_all_animators(em, PROBE_DT);
            printf("anim events %s count %d\n", labels[i], probe_event_count);
        }
    } else if (!strcmp(which, "import")) {
        // The committed walk clip binds onto the puppet by exact name.
        int n = load_animations_from_file(scene, skel, "assets/strut_walk.fbx", false, NULL);
        Animation* clip = n > 0 ? scene_find_animation(scene, "strut_walk") : NULL;
        if (!clip) {
            fprintf(stderr, "anim-probe: assets/strut_walk.fbx did not load\n");
            return 1;
        }
        int matched = 0;
        for (size_t i = 0; i < clip->channel_count; i++)
            matched += clip->channels[i].bone_index >= 0 ? 1 : 0;
        // `anim <case> <label> <key> <numbers>`, the one shape every probe line
        // takes -- the audio probe's too, so one regex reads them all.
        printf("anim import clip matched %d\n", matched);
        printf("anim import clip channels %zu\n", clip->channel_count);
        printf("anim import clip seconds %.3f\n", clip->duration / clip->ticks_per_second);
    } else {
        fprintf(stderr, "anim-probe: unknown case '%s'\n", which);
        rc = 1;
    }
    return rc;
}

int main(int argc, const char* argv[]) {
    printf("=== Physics Test ===\n\n");

    // Parse command line arguments. The HDR path stays POSITIONAL, which is the
    // whole interface this app had before it grew flags -- anything not
    // recognised below is still taken as the environment.
    bool headless = false;
    bool force_taa = false;
    int msaa = 0;
    int frames = 0;
    int screenshot_every = 0;
    const char* screenshot = NULL;
    const char* pad_script = NULL;
    const char* gamepad_db = NULL;
    const char* audio_probe = NULL;
    const char* audio_file = NULL;
    const char* anim_probe = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-x") || !strcmp(a, "--headless")) {
            headless = true;
        } else if (!strcmp(a, "--taa")) {
            force_taa = true;
        } else if ((!strcmp(a, "-f") || !strcmp(a, "--frames")) && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if ((!strcmp(a, "-S") || !strcmp(a, "--screenshot")) && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strcmp(a, "--screenshot-every") && i + 1 < argc) {
            screenshot_every = atoi(argv[++i]);
        } else if (!strcmp(a, "--msaa") && i + 1 < argc) {
            msaa = atoi(argv[++i]);
        } else if (!strcmp(a, "--trace-player")) {
            trace_player = true;
        } else if (!strcmp(a, "--trace-every") && i + 1 < argc) {
            trace_every = atoi(argv[++i]);
            if (trace_every < 1)
                trace_every = 1;
        } else if (!strcmp(a, "--pad-script") && i + 1 < argc) {
            pad_script = argv[++i];
        } else if (!strcmp(a, "--gamepad-db") && i + 1 < argc) {
            gamepad_db = argv[++i];
        } else if (!strcmp(a, "--mute")) {
            audio_muted = true;
        } else if (!strcmp(a, "--audio-probe") && i + 1 < argc) {
            audio_probe = argv[++i];
        } else if (!strcmp(a, "--audio-file") && i + 1 < argc) {
            audio_file = argv[++i];
        } else if (!strcmp(a, "--no-puppet")) {
            no_puppet = true;
        } else if (!strcmp(a, "--puppet") && i + 1 < argc) {
            puppet_path = argv[++i];
        } else if (!strcmp(a, "--twin") && i + 1 < argc) {
            twin_clip = argv[++i];
        } else if (!strcmp(a, "--anim-probe") && i + 1 < argc) {
            anim_probe = argv[++i];
        } else if (!strcmp(a, "--print-bindings")) {
            input_print_actions(actions, ACTION_COUNT);
            return 0;
        } else if (a[0] == '-') {
            // A dash-led token is never a path. Without this a typo'd flag,
            // or a value flag in final position whose guard above just
            // failed, becomes the environment path and the run reports
            // "Using HDR environment: --msaa".
            fprintf(stderr, "gametest: unknown or incomplete option '%s'\n", a);
            return -1;
        } else {
            hdr_path = a;
            printf("Using HDR environment: %s\n\n", hdr_path);
        }
    }

    // A self-contained offline render: a headless game, the offline audio
    // system, no window loop. It prints its measurements and exits, which is
    // what the `audio` gate reads.
    if (audio_probe) {
        GameConfig probe_config = {.engine = {.title = "audio-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "audio-probe: could not create game\n");
            return -1;
        }
        int rc = run_audio_probe(probe_game, audio_probe, audio_file);
        free_game(probe_game);
        return rc;
    }

    // The same shape for the animator: a headless game, the rig loaded, the
    // components ticked by the loop's own function, no window.
    if (anim_probe) {
        GameConfig probe_config = {.engine = {.title = "anim-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "anim-probe: could not create game\n");
            return -1;
        }
        int rc = run_anim_probe(probe_game, anim_probe);
        free_game(probe_game);
        return rc;
    }

    printf("Controls (keyboard / gamepad):\n");
    printf("  WASD / left stick, dpad - Move the player (idle -> walk -> run as it speeds up)\n");
    printf("  Space / A - Jump\n");
    printf("  E / LB - Wave (the right arm, over whatever the legs are doing)\n");
    printf("  F / X - Spawn falling box\n");
    printf("  R / Y - Raycast downward from player\n");
    printf("  G / B - Print ground state\n");
    printf("  P / Start - Pause/unpause physics\n");
    printf("  Mouse drag - Orbit camera\n");
    printf("  Escape - Quit\n");
    printf("Audio: a beep on jump and spawn, footsteps in time with the stride, a looping\n");
    printf("       tone at the door (--mute to silence)\n");
    printf("\nWalk into the door (right side) to push it open!\n\n");

    srand(42); // Deterministic random for testing

    // Create game
    GameConfig config = {.engine = {.title = "Physics Test - JoltC Integration",
                                    .width = 1280,
                                    .height = 720,
                                    .headless = headless}};

    // TAA replaces MSAA rather than joining it. This app is rigid meshes on the
    // pbr program, so every surface writes a motion vector and the accumulator
    // has something honest to reproject -- which is what separates it from the
    // particle demo next door (spec 11.103).
    //
    // Headless keeps MSAA and skips TAA unless asked, because jitter plus a
    // history makes the frame sensitive to async load timing.
    if (!headless || force_taa) {
        config.engine.msaa_samples = 1;
        config.engine.taa = true;
    }
    // After the policy, so --taa --msaa 4 is expressible.
    if (msaa > 0)
        config.engine.msaa_samples = msaa;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "Failed to create game\n");
        return -1;
    }
    game->engine->exit_after_frames = frames;
    engine_set_screenshot_path(game->engine, screenshot);
    game->engine->screenshot_every = screenshot_every;

    // A refused script or mapping file is a failed run, not a run with an
    // idle pad: a gate reading the trace must never mistake one for the other.
    if (gamepad_db && !input_load_gamepad_mappings(gamepad_db)) {
        free_game(game);
        return -1;
    }
    if (pad_script && !input_set_pad_script(&game->input, pad_script)) {
        free_game(game);
        return -1;
    }
    input_bind(&game->input, actions, ACTION_COUNT);

    // Set mouse callback
    engine_set_mouse_button_callback(game->engine, mouse_button_callback);

    // Set game callbacks
    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_pre_render(game, on_pre_render);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    // Run the game
    game_run(game);

    // Cleanup
    free_game(game);

    printf("Goodbye!\n");
    return 0;
}
