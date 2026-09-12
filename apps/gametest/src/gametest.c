// Game Test - Demonstrates the CharacterController integration
//
// Press WASD to move the player
// Press Space to jump (only when grounded)
// Press F to spawn a falling box
// Press P to pause/unpause
// Press G to print ground state
// Press R to raycast downward
// Press Escape for the pause menu

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
#include "cetra/program.h"
#include "cetra/text.h"
#include "cetra/ui.h"
#include "ui_backdrop.h"
#include "cetra/game/game.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"
#include "cetra/game/character.h"
#include "cetra/game/audio.h"
#include "cetra/game/settings.h"
#include "cetra/game/save.h"
#include "cetra/game/animator_component.h"
#include "cetra/animator.h"
#include "cetra/import.h"
#include "cetra/ibl.h"
#include "cetra/particle_system.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"
#include "cetra/texture.h"

static MouseDragController* drag_controller = NULL;
static Entity* player_entity = NULL;
static Entity* door_entity = NULL;
static Constraint* door_hinge = NULL;
static ShaderProgram* pbr_shader = NULL;
static int box_count = 0;
static const char* hdr_path = NULL;
static SaveSystem* save_system = NULL;
/*
 * Edges taken once per FRAME and consumed by the next fixed step.
 *
 * Input is polled once a frame, outside the step loop, so every step of a
 * multi-step frame reads the same press -- one F5 on a frame that ran five
 * steps meant five whole saves, each with its own fsync. The flag is what makes
 * the edge happen once while the WORK still happens in the settled world a
 * fixed step is.
 */
static bool save_pending = false;
static bool load_pending = false;

// Animation (spec 12.1): the player is the procedural puppet on an ANIMATOR
// component -- idle, walk and run blended from the controller's post-solve
// speed, a jump one-shot, a wave on the masked override layer, footsteps from
// the clips' events. --no-puppet keeps the red box; --twin <clip> stands a
// second rig beside the player playing its own clip, which is the per-node
// pose seen from a game.
#define PLAYER_SPEED 10.0f
// How big the player is. The VISUAL and the collision capsule both come from
// this: scaling one alone either sinks the feet through the floor or leaves the
// character standing on nothing.
#define PLAYER_SCALE  2.0f
#define PLAYER_RADIUS (0.5f * PLAYER_SCALE)
#define PLAYER_HALF_H (0.5f * PLAYER_SCALE)
// Capsule centre to feet, derived rather than written down twice. The yaw
// rebuilds the rig transform every step, so both sites read this.
#define PLAYER_RIG_DROP (-(PLAYER_RADIUS + PLAYER_HALF_H))
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

// The chaser (a second puppet that hunts the player) and the hearts it earns.
// It is its own entity with its own Animator, which is what per-node poses buy:
// two rigs over ONE set of meshes, each posed independently in the same frame.
#define CHASER_SPEED 6.0f // slower than the player, so it is escapable
#define CATCH_RADIUS (1.6f * PLAYER_SCALE)
// A few distinct hearts, not a cloud: RATE over SECONDS is the whole count,
// so a burst is about five. Raising either turns it back into a puff.
#define HEART_RATE    8.0f
#define HEART_SECONDS 0.6f
static Entity* chaser_entity = NULL;
static Animator* chaser_animator = NULL;
static SceneNode* chaser_rig = NULL;
static float chaser_yaw = 0.0f;
static bool no_chaser = false;

static SceneNode* heart_node = NULL;
static ParticleModule* heart_spawn = NULL;
static float heart_timer = 0.0f;    // > 0 while a burst is emitting
static float catch_cooldown = 0.0f; // so one catch is one burst, not sixty

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

// The UI (spec 12.2). Declared up here rather than beside its functions because
// on_pre_render asks whether the menu owns the pointer, and that is well above
// them in this file.
static UISystem* ui_system = NULL;
static Font* ui_font = NULL;
// Four screens (spec 12.2, phase 7). The HUD sits at the BOTTOM of the stack
// for the whole run and the menus push above it, which works because capture is
// a property of the stack rather than of the top: ui_captures_input is true
// while any screen on it is modal, and the HUD is the one that is not.
static UIScreen* screen_main = NULL;
static UIScreen* screen_pause = NULL;
static UIScreen* screen_settings = NULL;
static UIScreen* screen_hud = NULL;
// The audio system is a local inside on_init; a menu callback takes only its
// own user pointer, so the handle it needs is a file static like the sounds.
static AudioSystem* ui_audio = NULL;
// The menu's own contribution to the pause state, so it can be applied on its
// edges and never stomp the pause the player asked for with P.
static bool ui_menu_paused = false;
// --ui-screen: which screen to open at startup, if any. A file static because
// the install runs from on_init, long after the flags were parsed.
static const char* ui_screen_at_start = NULL;
// --ui-focus: how many times to press "down" once a screen is up, so a golden
// can photograph the focus visual. Fed through the real navigation path rather
// than by setting focus directly, because a highlight proves nothing about a
// menu if navigation is the half that is broken.
static int ui_focus_steps = 0;
// The backdrop's own fragment stage. Owned by the engine's program cache once
// registered, like every other program in the tree.
static ShaderProgram* ui_backdrop_program = NULL;

/*
 * What the settings screen edits, and where it persists. The sliders bind
 * straight into this struct -- a control is a VIEW of the app's variable, so
 * there is nothing to read back -- and it is written out when the screen
 * closes.
 *
 * The bloom toggle is the exception that makes the point: it binds the
 * ENGINE's own field, because since 11.108 that is a plain bool and a copy to
 * mediate it would only be a second place for the answer to live.
 *
 * Every control here moves something observable. window_mode is in the file and
 * deliberately NOT on the screen: settings_apply cannot make it take effect
 * yet, and a control that changes when clicked and does nothing is the defect
 * this app already shipped once.
 */
static GameSettings ui_settings;
static char ui_settings_path[1024];
static bool ui_settings_have_path = false;
static bool ui_settings_dirty = false;
static int ui_tonemap = POSTFX_TONEMAP_NEUTRAL;

// The HUD's two labels, rewritten from live state each frame.
static UIElement* hud_speed_label = NULL;
static UIElement* hud_anim_label = NULL;
// The player's POST-SOLVE ground speed, latched where the animator's knob is
// derived from it, so the number on screen and the rig cannot disagree.
static float hud_ground_speed = 0.0f;

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

    /*
     * Quicksave and quickload, as ordinary game actions rather than keys read
     * somewhere by hand -- so they rebind like everything else and a pad can
     * reach them. NOT flagged `ui`, which means a menu suppresses them: saving
     * from inside a pause screen would capture the menu's own state as part of
     * the world, and the screen stack is not what a save is about.
     */
    {"quicksave", {INPUT_KEY(F5, 1), INPUT_PAD(RIGHT_BUMPER, 1)}},
    {"quickload", {INPUT_KEY(F9, 1), INPUT_PAD(RIGHT_THUMB, 1)}},

    /*
     * The UI's own, flagged so they keep reading while the menu has taken input
     * away from the game -- the key that opens a menu has to be able to close
     * it. Escape rather than a quit: a menu is what Escape does in a game, and
     * quitting is an item inside it.
     *
     * BACK on the pad, not START: `pause` already holds START, and a button
     * doing two things is a bug waiting for whichever reader runs first.
     */
    {"menu", {INPUT_KEY(ESCAPE, 1), INPUT_PAD(BACK, 1)}, true},
    {"ui_up", {INPUT_KEY(UP, 1), INPUT_PAD(DPAD_UP, 1)}, true},
    {"ui_down", {INPUT_KEY(DOWN, 1), INPUT_PAD(DPAD_DOWN, 1)}, true},
    {"ui_left", {INPUT_KEY(LEFT, 1), INPUT_PAD(DPAD_LEFT, 1)}, true},
    {"ui_right", {INPUT_KEY(RIGHT, 1), INPUT_PAD(DPAD_RIGHT, 1)}, true},
    {"ui_accept", {INPUT_KEY(ENTER, 1), INPUT_PAD(A, 1)}, true},
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

/*
 * One crate, from explicit arguments.
 *
 * BOTH ways in go through here -- the keypress rolls the arguments, the loader
 * reads them back from the file -- so a restored crate is built by the same
 * code that built the original rather than by a second copy free to drift from
 * it. Splitting this out is what makes the crates saveable at all: the size and
 * colour used to be rolled inline and then stored only inside a Material and a
 * Jolt shape, where nothing could read them back, and the rand01 sequence that
 * produced them cannot be replayed without a draw count nobody keeps.
 */
static Entity* spawn_box(Game* game, const char* name, vec3 pos, float half, vec3 color) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);

    if (!em || !physics || !scene)
        return NULL;

    Entity* box = create_entity(em, name);
    if (!box)
        return NULL;
    glm_vec3_copy(pos, box->position);

    vec3 half_extents = {half, half, half};
    SceneNode* node = create_box_node(scene, half_extents, color, false);
    node_set_name(node, name);
    box->node = node;

    // Add physics body (density ~50 kg/m3, like a light wooden crate)
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX, .box.half_extents = {half, half, half}, .density = 50.0f};
    entity_add_rigid_body(box, physics, &shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);
    return box;
}

// The recipe's arguments, in the shape the save file carries them.
static cJSON* box_params(vec3 pos, float half, vec3 color) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return NULL;
    const double p[3] = {pos[0], pos[1], pos[2]};
    const double c[3] = {color[0], color[1], color[2]};
    cJSON_AddItemToObject(params, "position", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddNumberToObject(params, "half", half);
    cJSON_AddItemToObject(params, "color", cJSON_CreateDoubleArray(c, 3));
    return params;
}

static bool params_vec3(const cJSON* params, const char* key, vec3 out) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 3)
        return false;
    for (int i = 0; i < 3; i++) {
        const cJSON* e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(e))
            return false;
        out[i] = (float)e->valuedouble;
    }
    return true;
}

/*
 * The registered "box" recipe. The position it reads is only a starting point:
 * the entity's own saved pose is applied over it a moment later, which is what
 * puts a restored crate where it actually was rather than where it first fell.
 */
static Entity* spawn_box_from_save(EntityManager* em, const char* name, const cJSON* params,
                                   void* user) {
    (void)em;
    Game* game = (Game*)user;
    vec3 pos = {0.0f, 0.0f, 0.0f};
    vec3 color = {0.5f, 0.5f, 0.5f};
    if (!params || !params_vec3(params, "position", pos) || !params_vec3(params, "color", color)) {
        fprintf(stderr, "gametest: box params are malformed; '%s' is not restored\n", name);
        return NULL;
    }
    const cJSON* half = cJSON_GetObjectItemCaseSensitive(params, "half");
    if (!cJSON_IsNumber(half)) {
        fprintf(stderr, "gametest: box '%s' has no size; not restored\n", name);
        return NULL;
    }
    return spawn_box(game, name, pos, (float)half->valuedouble, color);
}

// Spawn a falling box at a random position above the scene
static void spawn_falling_box(Game* game) {
    // The counter advances only once the crate exists. It is SAVED state now,
    // so a refused spawn that burned a name would leave a permanent gap in what
    // the file carries.
    char name[32];
    snprintf(name, sizeof(name), "box_%d", box_count);

    // Random position above the scene, random colour, random size
    float x = (rand01() - 0.5f) * 20.0f;
    float z = (rand01() - 0.5f) * 20.0f;
    vec3 pos = {x, 15.0f + rand01() * 5.0f, z};
    vec3 color = {0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f};
    float half = 0.5f + rand01() * 1.0f;

    if (!spawn_box(game, name, pos, half, color))
        return;
    box_count++;

    // Recorded as it happens, by the only code that holds these numbers.
    if (save_system)
        save_note_spawn(save_system, name, "box", box_params(pos, half, color));

    printf("Spawned %s at (%.1f, %.1f, %.1f)\n", name, pos[0], pos[1], pos[2]);
}

// Track if player is touching door this frame (declared before on_update uses it)
static bool player_touching_door = false;

/*
 * The state an entity walk cannot reach.
 *
 * A save that carries every entity still restores a world whose player faces
 * the wrong way, whose next crate collides with an existing name, and whose
 * door forgets it was mid-swing: a facing angle, a spawn counter and two
 * deferred flags live in file statics here and nowhere the engine can see.
 *
 * Addressed directly, because they are plain variables and save.h has a row for
 * exactly that. The two alternatives are both worse: gathering them into a
 * struct for the serializer's benefit would rewrite door, yaw and catch logic
 * across a file the anim, audio, ui and gamepad groups and two goldens all
 * read, and a generated accessor pair per variable states each type twice -- in
 * the row and in the cast -- with nothing checking the two agree.
 */
static const SaveField SAVE_APP_FIELDS[] = {
    SAVE_ROW_AT(SAVE_INT, "box_count", &box_count),
    SAVE_ROW_AT(SAVE_FLOAT, "player_yaw", &player_yaw),
    SAVE_ROW_AT(SAVE_FLOAT, "chaser_yaw", &chaser_yaw),
    SAVE_ROW_AT(SAVE_FLOAT, "heart_timer", &heart_timer),
    SAVE_ROW_AT(SAVE_FLOAT, "catch_cooldown", &catch_cooldown),
    SAVE_ROW_AT(SAVE_BOOL, "door_open_pending", &door_open_pending),
    SAVE_ROW_AT(SAVE_FLOAT, "door_open_velocity", &door_open_velocity),
    SAVE_ROW_AT(SAVE_BOOL, "player_touching_door", &player_touching_door),
};
#define SAVE_APP_COUNT   ((int)(sizeof(SAVE_APP_FIELDS) / sizeof(SAVE_APP_FIELDS[0])))
#define SAVE_APP_VERSION 1

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
// the facing yaw cannot live there -- they go on the inner node.
//
// `rig` must be a node of its own, never the scene root. It used to accept the
// root and move its CHILDREN, which was right only while the root held nothing
// but the model: the floor is added before the player, so that scooped up the
// floor and parented it to the character, and WASD drove the ground around with
// him. take_puppet_root does the wrapping at import instead, before anything
// else is in the scene.
static SceneNode* attach_rig(Scene* scene, Entity* entity, SceneNode* rig, float drop,
                             float scale) {
    SceneNode* holder = create_node();
    node_set_name(holder, entity->name);
    SceneNode* inner = create_node();
    node_set_name(inner, "rig");
    node_add_child(holder, inner);
    node_add_child(inner, rig);
    node_add_child(scene->root_node, holder);
    glm_translate_make(inner->original_transform, (vec3){0.0f, drop, 0.0f});
    glm_scale_uni(inner->original_transform, scale);
    entity->node = holder;
    return inner;
}

// The imported model as a node that is NOT the scene root.
//
// create_scene_from_model_path makes the file's own node the root (spec 11.107),
// and a root cannot be re-parented under one of its own descendants -- that is a
// cycle, and every recursive walk in the engine runs until the stack is gone.
// So its children move into a wrapper HERE, at import, while the scene still
// contains nothing else. Doing it later is the bug this replaced: the root's
// children by then include the floor.
static SceneNode* take_puppet_root(Scene* scene) {
    SceneNode* found = node_find(scene->root_node, "puppet");
    if (!found)
        return NULL;
    if (found != scene->root_node)
        return found;
    SceneNode* wrapper = create_node();
    node_set_name(wrapper, "puppet_rig");
    // Bounded rather than drained: node_add_child refuses a cycle, and a
    // `while (children_count)` on a refusal spins forever.
    for (size_t guard = scene->root_node->children_count;
         guard > 0 && scene->root_node->children_count > 0; guard--)
        node_add_child(wrapper, scene->root_node->children[0]);
    node_add_child(scene->root_node, wrapper);
    return wrapper;
}

// Hearts weave as they rise. Written as a module rather than reached for from
// curl noise, because the ask is ONE axis with a phase, and curl noise wanders
// on three -- it reads as drift, not as a wobble.
//
// The phase comes from the particle's own `seed`, which is stable for its whole
// life, so five hearts from one burst weave independently instead of swaying in
// lockstep. Amplitude grows with age so they leave the burst tightly and spread
// as they climb.
typedef struct HeartWobble {
    float amplitude; // metres of sway at full age
    float cycles;    // full left-right sweeps over a lifetime
} HeartWobble;

static void heart_wobble_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                             float dt, float t) {
    (void)t;
    const HeartWobble* w = (const HeartWobble*)m->params;
    ParticlePool* pool = e->pool;
    for (size_t i = begin; i < end; i++) {
        float life = pool->lifetime[i] > 0.0f ? pool->lifetime[i] : 1.0f;
        float u = pool->age[i] / life; // 0 at birth, 1 at death
        float phase = pool->seed[i] * 6.2831853f;
        // The DERIVATIVE of amplitude * sin(phase), so the sway composes with
        // the upward drift instead of fighting the integrator for position.
        float omega = w->cycles * 6.2831853f / life;
        float sway = w->amplitude * omega * cosf(phase + omega * pool->age[i]);
        pool->velocity[i][0] += sway * u * dt * 8.0f;
    }
}

static ParticleModule* particle_module_heart_wobble(float amplitude, float cycles) {
    HeartWobble* w = malloc(sizeof(HeartWobble));
    if (!w)
        return NULL;
    w->amplitude = amplitude;
    w->cycles = cycles;
    return create_particle_module("heart_wobble", PARTICLE_PHASE_UPDATE, heart_wobble_run, w);
}

// A heart, drawn into an RGBA sprite: the implicit curve
// (x^2 + y^2 - 1)^3 - x^2*y^3 <= 0, which is the standard one. Generated rather
// than shipped, like the puppet and the audio tones -- the demo carries no art.
static unsigned char* heart_pixels(int size) {
    unsigned char* px = malloc((size_t)size * size * 4);
    if (!px)
        return NULL;
    // The curve (x^2 + y^2 - 1)^3 - x^2*y^3 <= 0 spans about x in [-1.2, 1.2]
    // and y in [-1.35, 1.25]. The WINDOW has to contain all of that. The first
    // cut of this shifted y down by 0.25, which clipped the top off both lobes
    // and took the cleft with them -- and a heart without its cleft is a
    // pentagon, which is exactly what it drew.
    const float half = 1.45f;
    // Supersampled rather than distance-faded: the implicit function grows at
    // wildly different rates around the outline (fast at the point, slow at the
    // lobes), so a fixed f/0.35 falloff blurs one end while the other stays
    // hard. Coverage is uniform by construction.
    const int ss = 4;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            int hits = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    float u = ((float)i + ((float)sx + 0.5f) / ss) / (float)size;
                    float v = ((float)j + ((float)sy + 0.5f) / ss) / (float)size;
                    float x = (u * 2.0f - 1.0f) * half;
                    // Row 0 is uploaded first and lands at v = 0, which is the
                    // BOTTOM of the billboard -- so the first row has to be the
                    // heart's POINT. Writing +y up here draws it upside down.
                    float y = (v * 2.0f - 1.0f) * half;
                    float t = x * x + y * y - 1.0f;
                    if (t * t * t - x * x * y * y * y <= 0.0f)
                        hits++;
                }
            }
            unsigned char* p = &px[((size_t)j * size + i) * 4];
            p[0] = 255;
            p[1] = 58;
            p[2] = 96;
            p[3] = (unsigned char)((float)hits / (float)(ss * ss) * 255.0f + 0.5f);
        }
    }
    return px;
}

// One emitter, parked and silent until a catch moves it and turns it on.
static void create_hearts(Engine* engine, Scene* scene) {
    ShaderProgram* particle_prog = create_particle_program();
    if (!particle_prog)
        return;
    engine_add_program(engine, particle_prog);

    ParticleSystem* sys = create_particle_system("hearts");
    particle_system_set_backend(sys, create_cpu_particle_sim_backend());

    ParticleEmitter* em = create_particle_emitter("heart", 256);
    ParticleRenderer* r = create_billboard_particle_renderer(particle_prog);
    Texture* sprite = texture_load_memory_owned(scene->tex_pool, "heart_sprite", heart_pixels(96),
                                                96, 96, 4, texture_desc(true));
    if (sprite)
        billboard_renderer_set_sprite(r, sprite, 2.0f);
    // Unlit: a heart is an icon, not a surface, and lighting it makes it dim
    // whenever the character walks into shadow.
    billboard_renderer_set_lit(r, false);
    particle_emitter_set_renderer(em, r);

    heart_spawn = particle_module_spawn_rate(0.0f); // off until a catch
    particle_emitter_add_module(em, heart_spawn);
    particle_emitter_add_module(
        em, particle_module_init_box_location(
                (vec3){-1.1f * PLAYER_SCALE, 0.0f, -1.1f * PLAYER_SCALE},
                (vec3){1.1f * PLAYER_SCALE, 0.9f * PLAYER_SCALE, 1.1f * PLAYER_SCALE}));
    particle_emitter_add_module(em, particle_module_init_lifetime(1.1f, 1.9f));
    particle_emitter_add_module(em, particle_module_init_size(0.22f, 0.34f));
    particle_emitter_add_module(
        em, particle_module_init_color((vec4){1.0f, 0.25f, 0.45f, 1.0f}, 0.08f));
    // Up, and a little drag, so they rise and ease off rather than accelerate
    // out of frame.
    particle_emitter_add_module(em, particle_module_update_drift((vec3){0.0f, 1.4f, 0.0f}));
    particle_emitter_add_module(em, particle_module_heart_wobble(0.45f, 1.5f));
    particle_emitter_add_module(em, particle_module_update_rotation(-1.2f, 1.2f));
    particle_emitter_add_module(em, particle_module_update_integrate(0.96f));

    particle_system_add_emitter(sys, em);
    scene_add_particle_system(scene, sys); // the scene owns and ticks it

    heart_node = create_node();
    node_set_name(heart_node, "hearts");
    node_set_particle_system(heart_node, sys); // the node is the spawn frame
    node_add_child(scene->root_node, heart_node);
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
            puppet_root = take_puppet_root(scene);
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

    // The save system. Here rather than at startup because a spawner needs the
    // game it spawns into, and the app table's accessors need nothing at all --
    // the base is the game only because a table must address something.
    save_system = create_save_system(game);
    if (save_system) {
        save_register_table(save_system, "gametest", SAVE_APP_VERSION, SAVE_APP_FIELDS,
                            SAVE_APP_COUNT, game);
        save_register_spawner(save_system, "box", spawn_box_from_save, game);
    }

    // Audio: one device, two SFX beeps. Headless opens no device (offline).
    AudioSystem* audio = create_audio_system(engine->headless);
    if (audio) {
        game_set_audio_system(game, audio);
        ui_audio = audio;
        // The buses take the loaded settings here rather than at install: the
        // audio system is created in this callback, so at install there was
        // nothing to push them into.
        if (ui_system)
            settings_apply(&ui_settings, audio, NULL);
        if (audio_muted)
            audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
        jump_sound = audio_sound_from_tone(audio, 660.0f, AUDIO_BUS_SFX);
        spawn_sound = audio_sound_from_tone(audio, 180.0f, AUDIO_BUS_SFX);
        step_sound = audio_sound_from_tone(audio, 110.0f, AUDIO_BUS_SFX);
        if (step_sound)
            audio_sound_set_volume(step_sound, 0.4f);
    }

    // Create floor entity (static physics body)
    //
    // The collider is a box CENTRED on the entity, so the surface a body rests on is
    // half an extent above that centre -- and the drawn plane has to be lifted to meet
    // it. One constant, read by the visual and the collider both, because these were
    // two independent literals and had drifted: the floor was drawn half a metre below
    // the surface everything stood on. Nothing caught it because no golden photographs
    // a body against the floor, and the anim probes compare bone matrices on the CPU.
    const float floor_half_y = 0.5f;

    Entity* floor = create_entity(em, "floor");
    glm_vec3_copy((vec3){0, -floor_half_y, 0}, floor->position);

    // Floor visual, lifted onto the collider's top by that same half-extent
    SceneNode* floor_node = create_node();
    node_set_name(floor_node, "floor");
    Mesh* floor_mesh = create_mesh();
    Plane floor_plane = {.position = {0, floor_half_y, 0},
                         .width = 50.0f,
                         .depth = 50.0f,
                         .segments_w = 10,
                         .segments_d = 10};
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
        .box.half_extents = {25.0f, floor_half_y, 25.0f},
        .density = 0.0f // Static body
    };
    entity_add_rigid_body(floor, physics, &floor_shape, MOTION_STATIC, OBJ_LAYER_STATIC);
    printf("Floor created with static physics\n");

    // Create player entity with CharacterController
    player_entity = create_entity(em, "player");
    glm_vec3_copy((vec3){0, 2.0f * PLAYER_SCALE, 0}, player_entity->position);

    if (puppet_root) {
        // The puppet, its feet a capsule's half-height plus radius below the
        // entity, on the locomotion space; the box's colour and size are the
        // capsule's, which stays the physics body either way.
        player_rig = attach_rig(scene, player_entity, puppet_root, PLAYER_RIG_DROP, PLAYER_SCALE);
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
        vec3 player_size = {PLAYER_RADIUS, PLAYER_RADIUS + PLAYER_HALF_H, PLAYER_RADIUS};
        vec3 player_color = {0.8f, 0.2f, 0.2f};
        SceneNode* player_node = create_box_node(scene, player_size, player_color, false);
        node_set_name(player_node, "player");
        player_entity->node = player_node;
    }

    // Player character controller
    CharacterControllerConfig player_config = character_controller_default_config();
    player_config.capsule_radius = PLAYER_RADIUS;
    player_config.capsule_half_height = PLAYER_HALF_H;
    player_config.step_height = 0.4f * PLAYER_SCALE;
    player_config.max_strength = 200.0f; // Strong enough to push door

    CharacterController* cc =
        entity_add_character_controller(player_entity, physics, &player_config);
    if (cc) {
        character_controller_set_contact_callback(cc, on_player_contact, game);
    }
    printf("Player created with CharacterController\n");

    // The chaser: the same meshes, its own rig, its own Animator, its own
    // character controller. It hunts the player in on_update.
    if (puppet_root && !no_chaser) {
        SceneNode* rig = clone_rig(puppet_root);
        if (rig) {
            chaser_entity = create_entity(em, "chaser");
            // The far corner at the TOP LEFT of the view: the camera sits at
            // +Z looking at the origin, so screen-right is +X and screen-up is
            // -Z. At 6 m/s against the player's 10 it is a chase, not an ambush.
            glm_vec3_copy((vec3){-20.0f, 2.0f * PLAYER_SCALE, -20.0f}, chaser_entity->position);
            chaser_rig = attach_rig(scene, chaser_entity, rig, PLAYER_RIG_DROP, PLAYER_SCALE);

            CharacterControllerConfig cfg = character_controller_default_config();
            cfg.capsule_radius = PLAYER_RADIUS;
            cfg.capsule_half_height = PLAYER_HALF_H;
            cfg.step_height = 0.4f * PLAYER_SCALE;
            entity_add_character_controller(chaser_entity, physics, &cfg);

            chaser_animator = create_animator(scene->skeletons[0]);
            if (chaser_animator) {
                animator_play_space(chaser_animator, "locomotion", locomotion, 3, 0.0f, true);
                entity_add_animator(chaser_entity, chaser_animator);
            }
            printf("Chaser created -- run!\n");
        }
    }

    create_hearts(engine, scene);

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
            attach_rig(scene, twin, rig, 0.0f, PLAYER_SCALE);
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
    hud_ground_speed = ground_speed;
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
        glm_scale_uni(player_rig->original_transform, PLAYER_SCALE);
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

    // The chaser: steer flat toward the player at its own speed, blend its legs
    // from the speed it actually achieved, and face where it is going.
    CharacterController* chase_cc =
        chaser_entity ? entity_get_character_controller(chaser_entity) : NULL;
    if (chase_cc) {
        vec3 to_player;
        glm_vec3_sub(player_entity->position, chaser_entity->position, to_player);
        to_player[1] = 0.0f;
        float gap = glm_vec3_norm(to_player);

        vec3 chase_vel;
        character_controller_get_velocity(chase_cc, chase_vel);
        if (chaser_animator) {
            float speed = hypotf(chase_vel[0], chase_vel[2]) / PLAYER_SPEED;
            chaser_animator->param = speed > 1.0f ? 1.0f : speed;
        }
        if (chaser_rig && hypotf(chase_vel[0], chase_vel[2]) > 0.1f) {
            float target = atan2f(chase_vel[0], chase_vel[2]);
            float d = target - chaser_yaw;
            while (d > (float)M_PI)
                d -= 2.0f * (float)M_PI;
            while (d < -(float)M_PI)
                d += 2.0f * (float)M_PI;
            float k = (float)dt * 12.0f;
            chaser_yaw += d * (k > 1.0f ? 1.0f : k);
            glm_mat4_identity(chaser_rig->original_transform);
            glm_translate(chaser_rig->original_transform, (vec3){0.0f, PLAYER_RIG_DROP, 0.0f});
            glm_rotate_y(chaser_rig->original_transform, chaser_yaw,
                         chaser_rig->original_transform);
            glm_scale_uni(chaser_rig->original_transform, PLAYER_SCALE);
        }

        // Close in unless already on top of him, so it does not jitter against
        // the player's capsule once it arrives.
        if (gap > CATCH_RADIUS * 0.5f) {
            glm_vec3_scale(to_player, CHASER_SPEED / gap, to_player);
            chase_vel[0] = to_player[0];
            chase_vel[2] = to_player[2];
        } else {
            chase_vel[0] = 0.0f;
            chase_vel[2] = 0.0f;
        }
        chase_vel[1] -= gravity * (float)dt;
        character_controller_set_velocity(chase_cc, chase_vel);

        // Caught: hearts, once. The cooldown is what makes it one burst rather
        // than one per step for as long as the two overlap.
        if (catch_cooldown > 0.0f)
            catch_cooldown -= (float)dt;
        if (gap < CATCH_RADIUS && catch_cooldown <= 0.0f) {
            catch_cooldown = 2.0f;
            heart_timer = HEART_SECONDS;
            if (heart_spawn)
                particle_module_spawn_rate_set(heart_spawn, HEART_RATE);
            if (heart_node) {
                // Burst from between the two of them, at chest height.
                vec3 mid;
                glm_vec3_add(player_entity->position, chaser_entity->position, mid);
                glm_vec3_scale(mid, 0.5f, mid);
                node_set_position(heart_node, (vec3){mid[0], mid[1], mid[2]});
            }
            printf("Caught!\n");
            if (jump_sound)
                audio_sound_play(jump_sound);
        }
    }

    // The burst is a WINDOW, not a one-shot: the emitter keeps spawning for
    // HEART_SECONDS and then stops, and the hearts already alive finish rising.
    if (heart_timer > 0.0f) {
        heart_timer -= (float)dt;
        if (heart_timer <= 0.0f && heart_spawn)
            particle_module_spawn_rate_set(heart_spawn, 0.0f);
    }

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

    /*
     * Quicksave and quickload. The WORK is here, in the fixed step, because
     * on_update runs after the step's physics sync and writes a settled world
     * -- pre_render would catch the entity poses of one step against the bodies
     * of the next. The EDGE is not here: on_frame_input took it once for the
     * whole frame, and the flag is what carries it across.
     */
    if (save_pending) {
        save_pending = false;
        char path[1024];
        if (save_system && save_default_path(path, sizeof(path), "quick"))
            save_write(save_system, path);
    }
    if (load_pending) {
        load_pending = false;
        char path[1024];
        if (save_system && save_default_path(path, sizeof(path), "quick")) {
            const SaveLoadResult r = save_read(save_system, path);
            if (r.ok)
                printf("Loaded: %d entities, %d spawned, %d dropped\n", r.entities_restored,
                       r.entities_spawned,
                       r.dropped_missing_entity + r.dropped_unknown_spawner +
                           r.dropped_unknown_component);
        }
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
    // A menu owns the pointer while it is up: without this, clicking a button
    // also orbits the camera behind it. Asked of the INPUT layer, which already
    // holds the answer -- the frame-input pass set it from ui_captures_input --
    // rather than of the UI, which is a layer this callback should not need to
    // know about. It also composes: anything that suppresses input, menu or
    // not, gates the camera for free.
    if (drag_controller && app_can_process_3d_input(engine) && !input_is_suppressed(&game->input)) {
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

    free_save_system(save_system);
    save_system = NULL;
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

// A pad held at full left deflection with A down, through the same reader seam
// the `gamepad` group scripts. No file and no device: it exists so the capture
// arm can assert what suppression DOES to a real source, rather than asking the
// flag to repeat what it was just told.
static bool ui_probe_pad(void* ctx, int pad, GLFWgamepadstate* out) {
    (void)ctx;
    if (pad != 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1.0f;
    out->buttons[GLFW_GAMEPAD_BUTTON_A] = GLFW_PRESS;
    return true;
}

// The row in a given state, as an index, or -1. There is no focus accessor and
// none is needed: UIElement.state is public, settled by the input pass, and is
// the same value the drawing reads -- so this asks what the picture asks.
static float ui_probe_state_index(UIElement** rows, int n, UIState want) {
    for (int i = 0; i < n; i++)
        if (rows[i]->state == want)
            return (float)i;
    return -1.0f;
}

static void ui_probe_rect(const char* which, const char* label, UIRect r) {
    printf("ui %s %s rect %.6f %.6f %.6f %.6f\n", which, label, (double)r.x, (double)r.y,
           (double)r.w, (double)r.h);
}

// Three equal buttons in a fixed-width panel: the smallest tree that exercises
// FIXED, GROW, padding and spacing at once, and the one every arm below shares
// so a number that moves means the layout moved and not the fixture.
static UIElement* ui_probe_column(UIScreen* screen, UIElement** rows) {
    UIElement* panel = ui_panel(ui_screen_root(screen));
    panel->size_mode[0] = UI_FIXED;
    panel->size[0] = 300.0f;
    panel->padding[0] = panel->padding[1] = panel->padding[2] = panel->padding[3] = 10.0f;
    panel->spacing = 8.0f;
    rows[0] = ui_button(panel, "One", NULL, NULL);
    rows[1] = ui_button(panel, "Two", NULL, NULL);
    rows[2] = ui_button(panel, "Three", NULL, NULL);
    for (int i = 0; i < 3; i++)
        rows[i]->size_mode[0] = UI_GROW;
    return panel;
}

/*
 * The element tree as a pure function: layout, wrapping, geometric navigation,
 * hit-testing, capture, the stack and theme resolution, printed as numbers.
 *
 * It needs a headless engine for ONE thing -- the font, because FIT sizing is
 * made of measurement and measuring a string is what bakes its glyphs -- and it
 * never draws a frame. ui_layout is pure, ui_update takes a plain struct of
 * values rather than a device, so navigation and activation are driven here
 * with no window, no pad and no GPU.
 */
static int run_ui_screens_probe(Game* game, const char* which) {
    Engine* engine = game->engine;
    Font* font = load_font(engine->text_renderer->font_pool, "apps/splash/assets/Roboto-Bold.ttf",
                           64.0f, true);
    if (!font) {
        fprintf(stderr, "ui-probe: could not load the font\n");
        return 1;
    }
    UISystem* ui = create_ui_system(engine);
    if (!ui) {
        fprintf(stderr, "ui-probe: could not create the ui system\n");
        return 1;
    }
    ui_set_font(ui, font, 22.0f);

    const float font_size = 22.0f;
    int rc = 0;

    if (!strcmp(which, "layout")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_layout(s, 1280.0f, 720.0f);
        ui_probe_rect("layout", "panel", panel->rect);
        ui_probe_rect("layout", "row0", rows[0]->rect);
        ui_probe_rect("layout", "row1", rows[1]->rect);
        ui_probe_rect("layout", "row2", rows[2]->rect);
    } else if (!strcmp(which, "layout-resize")) {
        // The SAME tree at three sizes. Layout is a pure function of (tree,
        // width, height), so nothing may carry over between these three calls.
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_layout(s, 640.0f, 360.0f);
        ui_probe_rect("layout-resize", "small", panel->rect);
        ui_layout(s, 2560.0f, 1440.0f);
        ui_probe_rect("layout-resize", "large", panel->rect);
        // Back to the first size: the rects must be what they were, or layout
        // is accumulating something instead of deriving it.
        ui_layout(s, 640.0f, 360.0f);
        ui_probe_rect("layout-resize", "again", panel->rect);
    } else if (!strcmp(which, "wrap")) {
        const char* para = "the quick brown fox jumps over the lazy dog and keeps on running";
        const float limit = 400.0f;
        int lines = 0;
        const char* p = para;
        while (*p && lines < 16) {
            const size_t n = ui_text_wrap_point(font, font_size, 0.0f, p, limit);
            if (n == 0)
                break;
            char buf[256];
            const size_t copy = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
            memcpy(buf, p, copy);
            buf[copy] = '\0';
            char label[16];
            snprintf(label, sizeof(label), "line%d", lines);
            printf("ui wrap %s width %.6f\n", label,
                   (double)ui_text_width(font, font_size, 0.0f, buf));
            lines++;
            p += n;
            while (*p == ' ')
                p++;
        }
        printf("ui wrap lines count %.6f\n", (double)lines);
        printf("ui wrap limit points %.6f\n", (double)limit);
    } else if (!strcmp(which, "nav")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        ui_probe_column(s, rows);
        ui_push(ui, s);
        const UIInput idle = {0};
        ui_update(ui, &idle, 1280.0f, 720.0f);
        printf("ui nav start focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        UIInput down = {0};
        down.nav_down = true;
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav down focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav twice focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav last focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        // Off the end: focus wraps to the top rather than sticking there. This
        // is the FOURTH press and not the third, because nothing is focused to
        // begin with and the first press only acquires -- a detail worth the
        // line, since counting presses instead of moves reads as a broken wrap.
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav wrapped focus %.6f\n",
               (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        UIInput up = {0};
        up.nav_up = true;
        ui_update(ui, &up, 1280.0f, 720.0f);
        printf("ui nav up focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
    } else if (!strcmp(which, "hit")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_push(ui, s);
        const UIInput idle = {0};
        ui_update(ui, &idle, 1280.0f, 720.0f);

        // Point-in-rect against the rects layout settled: a control claims its
        // own point and none of its neighbours'.
        const float cx = rows[1]->rect.x + rows[1]->rect.w * 0.5f;
        const float cy = rows[1]->rect.y + rows[1]->rect.h * 0.5f;
        printf("ui hit centre row1 %.6f\n", ui_rect_hit(rows[1]->rect, cx, cy) ? 1.0 : 0.0);
        printf("ui hit centre row0 %.6f\n", ui_rect_hit(rows[0]->rect, cx, cy) ? 1.0 : 0.0);
        printf("ui hit centre row2 %.6f\n", ui_rect_hit(rows[2]->rect, cx, cy) ? 1.0 : 0.0);

        // A point in the panel's own padding belongs to the PARENT: inside the
        // panel, inside no row.
        const float px = panel->rect.x + 2.0f;
        const float py = panel->rect.y + 2.0f;
        printf("ui hit padding panel %.6f\n", ui_rect_hit(panel->rect, px, py) ? 1.0 : 0.0);
        int claimed = 0;
        for (int i = 0; i < 3; i++)
            claimed += ui_rect_hit(rows[i]->rect, px, py) ? 1 : 0;
        printf("ui hit padding rows %.6f\n", (double)claimed);

        // So does the spacing gap between two rows.
        const float gy = rows[0]->rect.y + rows[0]->rect.h + 2.0f;
        claimed = 0;
        for (int i = 0; i < 3; i++)
            claimed += ui_rect_hit(rows[i]->rect, cx, gy) ? 1 : 0;
        printf("ui hit gap rows %.6f\n", (double)claimed);

        // And the real pointer walk reaches the element under the cursor. The
        // test is that its state LEAVES normal rather than that it is hover
        // specifically: pointing at a control also focuses it, so the two
        // states are not alternatives and asking for one finds neither.
        UIInput at = {0};
        at.pointer_x = cx;
        at.pointer_y = cy;
        ui_update(ui, &at, 1280.0f, 720.0f);
        printf("ui hit pointer row1 %.6f\n", rows[1]->state != UI_STATE_NORMAL ? 1.0 : 0.0);
        printf("ui hit pointer row0 %.6f\n", rows[0]->state != UI_STATE_NORMAL ? 1.0 : 0.0);
    } else if (!strcmp(which, "capture")) {
        UIScreen* hud = ui_screen(ui, "hud");
        ui_screen_set_modal(hud, false);
        ui_panel(ui_screen_root(hud));
        UIScreen* menu = ui_screen(ui, "menu");
        UIElement* rows[3];
        ui_probe_column(menu, rows);

        ui_push(ui, hud);
        printf("ui capture hud captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_push(ui, menu);
        printf("ui capture menu captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_pop(ui);
        printf("ui capture popped captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_pop_all(ui);
        printf("ui capture cleared captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);

        /*
         * And what the switch actually DOES, through a real source.
         *
         * A scripted pad holds the left stick and A, so move_x -- a game action
         * -- and ui_accept -- flagged `ui` -- both read. Raising suppression
         * must take one to zero and leave the other alone, which is the whole
         * contract and the branch's most dangerous new global.
         *
         * Asserting input_is_suppressed instead, as this did, proves only that
         * a bool remembers what it was told: with no source held both actions
         * read zero either way, and the arm passes over a layer that has gone
         * completely deaf.
         */
        input_bind(&game->input, actions, ACTION_COUNT);
        input_set_pad_reader(&game->input, ui_probe_pad, NULL, NULL);

        input_update(&game->input);
        printf("ui capture free move %.6f\n", (double)input_action_value(&game->input, "move_x"));
        printf("ui capture free accept %.6f\n",
               (double)input_action_value(&game->input, "ui_accept"));

        input_set_suppressed(&game->input, true);
        input_update(&game->input);
        printf("ui capture held move %.6f\n", (double)input_action_value(&game->input, "move_x"));
        printf("ui capture held accept %.6f\n",
               (double)input_action_value(&game->input, "ui_accept"));

        input_set_suppressed(&game->input, false);
        input_update(&game->input);
        printf("ui capture given move %.6f\n", (double)input_action_value(&game->input, "move_x"));
    } else if (!strcmp(which, "stack")) {
        UIScreen* a = ui_screen(ui, "a");
        UIScreen* b = ui_screen(ui, "b");
        ui_panel(ui_screen_root(a));
        ui_panel(ui_screen_root(b));
        printf("ui stack empty top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
        ui_push(ui, a);
        printf("ui stack pushed top %.6f\n", ui_top(ui) == a ? 1.0 : 0.0);
        ui_push(ui, b);
        printf("ui stack second top %.6f\n", ui_top(ui) == b ? 1.0 : 0.0);
        ui_pop(ui);
        printf("ui stack popped top %.6f\n", ui_top(ui) == a ? 1.0 : 0.0);
        ui_push(ui, b);
        ui_pop_all(ui);
        printf("ui stack cleared top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
        // Popping an empty stack is a no-op, not an underflow.
        ui_pop(ui);
        printf("ui stack underflow top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
    } else if (!strcmp(which, "theme-identity")) {
        // Two identical trees, one of which sets a ZEROED style on every row.
        // If zero really means inherit, the two emit the same vertices; if it
        // means "black, no padding, no radius", they cannot.
        UIScreen* plain = ui_screen(ui, "plain");
        UIElement* plain_rows[3];
        ui_probe_column(plain, plain_rows);

        UIScreen* zeroed = ui_screen(ui, "zeroed");
        UIElement* zero_rows[3];
        ui_probe_column(zeroed, zero_rows);
        const UIStyle nothing = {0};
        for (int i = 0; i < 3; i++)
            ui_set_style(zero_rows[i], &nothing);

        UIDrawList* dl = ui_draw_list(ui);
        ui_push(ui, plain);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_plain = ui_draw_list_signature(dl);
        ui_pop(ui);

        ui_push(ui, zeroed);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_zeroed = ui_draw_list_signature(dl);
        ui_pop(ui);

        printf("ui theme-identity styles match %.6f\n", sig_plain == sig_zeroed ? 1.0 : 0.0);
        // A style that says something must NOT hash alike, or the comparison
        // above would pass on a signature that ignores style entirely.
        UIStyle loud = {0};
        loud.corner_radius = 14.0f;
        loud.bg[0] = loud.bg[3] = 1.0f;
        for (int i = 0; i < 3; i++)
            ui_set_style(zero_rows[i], &loud);
        ui_push(ui, zeroed);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_loud = ui_draw_list_signature(dl);
        ui_pop(ui);
        printf("ui theme-identity styled differs %.6f\n", sig_loud != sig_plain ? 1.0 : 0.0);
    } else {
        fprintf(stderr, "ui-probe: unknown case '%s'\n", which);
        rc = 1;
    }

    free_ui_system(ui);
    return rc;
}

// --ui-probe (spec 12.2): the game-layer state a menu edits, checked with no
// window, no GL and no audio device. It runs BEFORE the engine is created
// rather than inside a headless game the way the audio and anim probes do,
// because none of it needs one -- settings.c is cJSON over a struct and an
// offline AudioSystem opens nothing. That is what lets the `ui` gate assert on
// it while the no-GPU-during-gates rule holds.
static void ui_probe_print(const char* label, const GameSettings* s) {
    printf("ui settings %s master %.6f\n", label, s->master_volume);
    printf("ui settings %s music %.6f\n", label, s->music_volume);
    printf("ui settings %s sfx %.6f\n", label, s->sfx_volume);
    printf("ui settings %s ui %.6f\n", label, s->ui_volume);
    printf("ui settings %s window_mode %.6f\n", label, (double)s->window_mode);
    printf("ui settings %s vsync %.6f\n", label, s->vsync ? 1.0 : 0.0);
}

static int run_ui_probe(const char* which) {
    if (strcmp(which, "settings") != 0) {
        fprintf(stderr, "ui-probe: unknown case '%s'\n", which);
        return 1;
    }

    // The platform path is part of what is under test, so the probe resolves it
    // rather than taking one on the command line. CETRA_SETTINGS_DIR is what
    // keeps a run out of the real user directory.
    char path[1024];
    if (!settings_default_path(path, sizeof(path))) {
        fprintf(stderr, "ui-probe: could not resolve a settings path\n");
        return 1;
    }

    GameSettings written;
    settings_defaults(&written);
    ui_probe_print("default", &written);

    written.master_volume = 0.25f;
    written.music_volume = 0.5f;
    written.sfx_volume = 0.75f;
    written.ui_volume = 0.125f;
    written.window_mode = SETTINGS_WINDOW_FULLSCREEN;
    written.vsync = false;
    if (!settings_save(&written, path)) {
        fprintf(stderr, "ui-probe: save failed\n");
        return 1;
    }

    // Zeroed and not defaulted before the read: against a struct that already
    // holds the answer, a loader that merely leaves a field alone passes.
    GameSettings back;
    memset(&back, 0, sizeof(back));
    if (!settings_load(&back, path)) {
        fprintf(stderr, "ui-probe: load failed\n");
        return 1;
    }
    ui_probe_print("roundtrip", &back);

    // The enum rides the file as a NAME. Read back as a number it would still
    // round-trip while an inserted enumerator silently re-pointed every file a
    // player already has, so the name is the half worth asserting.
    bool named = false;
    FILE* file = fopen(path, "rb");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "fullscreen")) {
                named = true;
                break;
            }
        }
        fclose(file);
    }
    printf("ui settings named fullscreen %.6f\n", named ? 1.0 : 0.0);

    // An absent file is the first run: defaults, not zeros. Zero is silence and
    // a windowed mode, which is a plausible-looking wrong answer.
    GameSettings absent;
    memset(&absent, 0, sizeof(absent));
    settings_load(&absent, "gametest-no-such-settings.json");
    ui_probe_print("absent", &absent);

    // What apply actually pushes. The getter is the only way to ask: miniaudio
    // has no master get, so a settings screen reads what the setter recorded.
    AudioSystem* audio = create_audio_system(true);
    if (!audio) {
        fprintf(stderr, "ui-probe: could not create the offline audio system\n");
        return 1;
    }
    settings_apply(&back, audio, NULL);
    printf("ui settings applied master %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_MASTER));
    printf("ui settings applied music %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_MUSIC));
    printf("ui settings applied sfx %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_SFX));
    printf("ui settings applied ui %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_UI));
    free_audio_system(audio);
    return 0;
}

/*
 * The screens (spec 12.2, phase 7): a main menu, a pause menu, a settings
 * screen and a HUD. They replace phase 2's --ui-smoke, whose job was to put the
 * draw primitives in front of a pixel comparison before anything was built on
 * them, because every way they can be wrong (a glyph mirrored about its own
 * baseline, the scissor's point-to-pixel conversion, the corner SDF, the blend
 * func) is invisible to a compile and obvious in a picture.
 *
 * Nothing opens at startup unless --ui-screen says so. A main menu up by
 * default would pause the sim before the first frame, which is the state every
 * scripted-pad and trace run in the gate suite would then be driving against.
 */
// ESCAPE HATCH 1: a custom-drawn element, the master volume as a segmented
// meter painted straight into the draw list. It gets the same rect the layout
// settled and the same focus and hit-testing as a built-in kind -- ui_set_draw
// replaces the drawing and nothing else.
static void ui_draw_meter(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    const int segments = 12;
    const float gap = 3.0f;
    const float w = (el->rect.w - gap * (float)(segments - 1)) / (float)segments;
    const int lit = (int)(ui_settings.master_volume * (float)segments + 0.5f);
    for (int i = 0; i < segments; i++) {
        const UIRect seg = {el->rect.x + (w + gap) * (float)i, el->rect.y, w, el->rect.h};
        vec4 on = {0.45f, 0.70f, 1.00f, 1.0f};
        vec4 off = {0.20f, 0.21f, 0.26f, 1.0f};
        ui_draw_rounded(dl, seg, 2.0f, i < lit ? on : off, NULL, 0.0f);
    }
}

// Every bound control reaches real state. A callback runs AFTER the value has
// been written, so it reads the new one -- and because the sliders bind into
// ui_settings directly, ONE handler pushing the whole struct does the same work
// as four that each have to know their own bus.
static void ui_settings_changed(UIElement* el, void* user) {
    (void)el;
    ui_settings_dirty = true;
    settings_apply(&ui_settings, ui_audio, user);
}

static void ui_tonemap_changed(UIElement* el, void* user) {
    (void)el;
    Engine* engine = user;
    if (engine && engine->postfx)
        engine->postfx->tonemap_mode = ui_tonemap;
}

// Resume and Back are the same act -- close the screen on top -- so they are
// the same function under two labels. The frame-input pass then hands input and
// the sim back on its own, because it reads ui_captures_input every frame
// rather than on an edge.
static void ui_action_close(UIElement* el, void* user) {
    (void)el;
    (void)user;
    ui_pop(ui_system);
}

// New Game closes every menu WITHOUT taking the HUD with them, which is what
// ui_pop_all would do: the HUD is a screen like any other and lives at the
// bottom of the same stack.
static void ui_action_new_game(UIElement* el, void* user) {
    (void)el;
    (void)user;
    while (ui_top(ui_system) && ui_top(ui_system) != screen_hud)
        ui_pop(ui_system);
}

static void ui_action_open_settings(UIElement* el, void* user) {
    (void)el;
    (void)user;
    ui_push(ui_system, screen_settings);
}

// Quitting is a menu item, which is where it belongs -- not a key.
static void ui_action_quit(UIElement* el, void* user) {
    (void)el;
    Engine* engine = user;
    if (engine && engine->window)
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
}

// Sets a label's text only when it actually changed: ui_set_text owns a copy,
// and a HUD rewriting two strings sixty times a second for the same characters
// is allocation with nothing to show for it.
static void hud_set_text(UIElement* el, const char* text) {
    if (el && (!el->text || strcmp(el->text, text) != 0))
        ui_set_text(el, text);
}

/*
 * Escape opens the menu and closes it again, which is what it does in every
 * game that has one; quitting is a menu item, not a key. The action is flagged
 * `ui`, so it keeps reading while the menu itself has taken input away from the
 * game -- otherwise the key that opened the menu could not close it.
 */
static void on_frame_input(Game* game) {
    /*
     * Once per FRAME, which is the whole reason these are taken here. on_update
     * runs once per fixed STEP, and input is polled outside that loop -- so a
     * frame that ran five steps read the same press five times, and one F5 was
     * five entire saves with five fsyncs. Only the EDGE moves here; the work
     * still happens in the step, against the settled world.
     *
     * Above the ui_system guard, because --no-ui is a game that can still save.
     */
    if (input_action_pressed(&game->input, "quicksave"))
        save_pending = true;
    if (input_action_pressed(&game->input, "quickload"))
        load_pending = true;

    if (!ui_system)
        return;

    // The HUD is the non-modal case, and it is what proves a screen coexists
    // with gameplay: it draws every frame over a live sim and takes nothing.
    char line[64];
    snprintf(line, sizeof(line), "speed  %.2f", (double)hud_ground_speed);
    hud_set_text(hud_speed_label, line);
    snprintf(line, sizeof(line), "blend  %.2f",
             player_animator ? (double)player_animator->param : 0.0);
    hud_set_text(hud_anim_label, line);

    // The app decides only when a menu OPENS; closing is the layer's, through
    // UIInput.back. Doing both here left ui_pop with no caller and the back
    // field permanently false -- and wiring the field without removing this
    // would pop twice for one press, once in each place.
    //
    // "Open" means a MENU, not any screen: the HUD is on the stack for the
    // whole run, so a bare ui_top test would read as a menu already being up
    // and Escape would never open one.
    const bool menu_pressed = input_action_pressed(&game->input, "menu");
    const bool menu_open = ui_top(ui_system) && ui_top(ui_system) != screen_hud;
    if (menu_pressed && !menu_open && screen_pause)
        ui_push(ui_system, screen_pause);

    // One synthetic "down" per frame while --ui-focus has any left, so a golden
    // can be taken with the focus somewhere other than where it lands.
    bool forced_down = false;
    if (ui_focus_steps > 0 && menu_open) {
        forced_down = true;
        ui_focus_steps--;
    }

    double mx = 0.0, my = 0.0;
    input_mouse_pos(&game->input, &mx, &my);
    const UIInput in = {
        .pointer_x = (float)mx,
        .pointer_y = (float)my,
        .pointer_down = input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_pressed = input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_released = input_mouse_released(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .nav_up = input_action_pressed(&game->input, "ui_up"),
        .nav_down = input_action_pressed(&game->input, "ui_down") || forced_down,
        .nav_left = input_action_pressed(&game->input, "ui_left"),
        .nav_right = input_action_pressed(&game->input, "ui_right"),
        .accept = input_action_pressed(&game->input, "ui_accept"),
        .back = menu_pressed && menu_open,
    };
    ui_update(ui_system, &in, (float)game->engine->win_width, (float)game->engine->win_height);

    // The file is written when the settings screen closes by ANY route -- its
    // own Back button, Escape, or a pop from somewhere else. In the button's
    // callback it would miss the other two; on each slider callback it would
    // rewrite the file once per frame of a drag.
    if (ui_settings_dirty && ui_top(ui_system) != screen_settings) {
        ui_settings_dirty = false;
        if (ui_settings_have_path)
            settings_save(&ui_settings, ui_settings_path);
    }

    // The one line that stops the character walking while a menu is up. Set
    // every frame rather than on the edges, so a screen closed by any route --
    // a button, a callback, a scene change -- gives input back.
    const bool captured = ui_captures_input(ui_system);
    input_set_suppressed(&game->input, captured);

    // The menu pauses the sim on its EDGES, and through the pause API rather
    // than by storing the field. Assigning it every frame made this the only
    // writer that mattered: the P key toggles pause from on_pre_render, later
    // in the same frame, and the next frame's assignment put it straight back
    // -- so the pause action lasted one frame and appeared dead.
    if (captured != ui_menu_paused) {
        ui_menu_paused = captured;
        if (captured)
            game_pause(game);
        else
            game_unpause(game);
    }
}

// A menu's column, styled the same way on each screen that has one.
static UIElement* ui_menu_panel(UIScreen* screen, const char* title) {
    UIElement* panel = ui_panel(ui_screen_root(screen));
    panel->align_cross = UI_ALIGN_CENTER;
    panel->size_mode[0] = UI_FIXED;
    panel->size[0] = 320.0f;

    // A per-element style: exactly the three-level chain the header describes,
    // used at its first level. Everything left at zero -- the colours, the
    // padding -- still comes from the theme and then the engine default, so
    // this says only what it means to change.
    UIElement* heading = ui_label(panel, title);
    const UIStyle heading_style = {
        .font_size = 30.0f,
        .tracking = 1.5f,
        .line_spacing = 1.15f,
        .fg = {0.96f, 0.97f, 1.00f, 1.0f},
        .padding = {6.0f, 0.0f, 14.0f, 0.0f},
    };
    ui_set_style(heading, &heading_style);
    return panel;
}

// GROW on the cross axis, so every control is the panel's width. Left at FIT
// each one hugs its own text and the column comes out ragged. The heading at
// index 0 is skipped: GROWn it is left-aligned text in a wide box, where FIT
// under the panel's centre alignment is the centred title this wants.
static void ui_rows_grow(UIElement* panel) {
    for (size_t i = 1; i < panel->child_count; i++)
        panel->children[i]->size_mode[0] = UI_GROW;
}

static bool ui_install(Engine* engine) {
    ui_font = load_font(engine->text_renderer->font_pool, "apps/splash/assets/Roboto-Bold.ttf",
                        64.0f, true);
    if (!ui_font) {
        fprintf(stderr, "ui: could not load the font\n");
        return false;
    }
    ui_system = create_ui_system(engine);
    if (!ui_system) {
        fprintf(stderr, "ui: could not create the ui system\n");
        return false;
    }
    ui_set_font(ui_system, ui_font, 22.0f);

    // The player's own file, read before a control is built so every slider
    // opens at the value it is about to edit. An absent file is the first run
    // and leaves defaults; the audio half is pushed from on_init, where the
    // audio system exists.
    if (settings_default_path(ui_settings_path, sizeof(ui_settings_path))) {
        ui_settings_have_path = true;
        settings_load(&ui_settings, ui_settings_path);
    } else {
        settings_defaults(&ui_settings);
    }
    settings_apply(&ui_settings, NULL, engine);

    // The app carries its own GLSL, the apps/network precedent. Registered with
    // the engine's cache so the cache owns it; a failure is logged there and
    // leaves the backdrop as an ordinary panel rather than taking the menu down.
    ui_backdrop_program =
        create_program_from_source("ui_backdrop", UI_BACKDROP_VERT, UI_BACKDROP_FRAG, NULL);
    if (ui_backdrop_program)
        engine_add_program(engine, ui_backdrop_program);

    // The MAIN menu: the one screen with a backdrop, because it is the one
    // drawn over nothing in particular. It fades rather than slides, which is
    // what a game's first screen does.
    screen_main = ui_screen(ui_system, "main");
    ui_screen_set_modal(screen_main, true);
    ui_screen_transition(screen_main, UI_TRANSITION_FADE, 0.20f);

    // ESCAPE HATCH 2: a backdrop with its own fragment stage, filling the
    // screen behind the panel. This is the layer's headline claim -- that a
    // menu is not limited to what the element vocabulary can express.
    UIElement* backdrop = ui_panel(ui_screen_root(screen_main));
    backdrop->fill = true; // out of the flow, covering the screen
    if (ui_backdrop_program)
        ui_set_element_program(backdrop, ui_backdrop_program);

    UIElement* main_panel = ui_menu_panel(screen_main, "CETRA");
    ui_button(main_panel, "New Game", ui_action_new_game, NULL);
    ui_button(main_panel, "Settings", ui_action_open_settings, NULL);
    ui_button(main_panel, "Quit", ui_action_quit, engine);
    ui_rows_grow(main_panel);

    // The PAUSE menu, deliberately with NO backdrop: it is drawn over live
    // gameplay, and hiding the thing the player paused defeats the point. The
    // scene keeps rendering because pausing stops only the fixed step.
    screen_pause = ui_screen(ui_system, "pause");
    ui_screen_set_modal(screen_pause, true);
    ui_screen_transition(screen_pause, UI_TRANSITION_SLIDE, 0.18f);
    UIElement* pause_panel = ui_menu_panel(screen_pause, "Paused");
    ui_button(pause_panel, "Resume", ui_action_close, NULL);
    ui_button(pause_panel, "Settings", ui_action_open_settings, NULL);
    ui_button(pause_panel, "Quit", ui_action_quit, engine);
    ui_rows_grow(pause_panel);

    // SETTINGS. Every control moves something the player can hear or see: the
    // three sliders are the buses, VSync is the swap interval, Tonemap is the
    // live curve. Bloom binds the ENGINE's own bool with no callback at all,
    // which is the 11.108 dividend -- the field IS the setting, so there is
    // nothing for a handler to forward.
    screen_settings = ui_screen(ui_system, "settings");
    ui_screen_set_modal(screen_settings, true);
    ui_screen_transition(screen_settings, UI_TRANSITION_SLIDE, 0.18f);
    UIElement* set_panel = ui_menu_panel(screen_settings, "Settings");
    ui_slider(set_panel, "Master", 0.0f, 1.0f, &ui_settings.master_volume, ui_settings_changed,
              engine);
    ui_slider(set_panel, "Music", 0.0f, 1.0f, &ui_settings.music_volume, ui_settings_changed,
              engine);
    ui_slider(set_panel, "SFX", 0.0f, 1.0f, &ui_settings.sfx_volume, ui_settings_changed, engine);
    if (engine->postfx)
        ui_toggle(set_panel, "Bloom", &engine->postfx->bloom_enabled, NULL, NULL);
    ui_toggle(set_panel, "VSync", &ui_settings.vsync, ui_settings_changed, engine);
    static const char* const modes[] = {"Passthrough", "ACES", "Neutral", "AgX", "Linear"};
    ui_tonemap = engine->postfx ? engine->postfx->tonemap_mode : POSTFX_TONEMAP_NEUTRAL;
    ui_selector(set_panel, "Tonemap", modes, 5, &ui_tonemap, ui_tonemap_changed, engine);

    // ESCAPE HATCH 1 on screen: an element the app paints itself, which still
    // lays out, still takes focus and still takes a click -- dropping out of
    // the element vocabulary costs only the drawing.
    UIElement* meter = ui_panel(set_panel);
    ui_set_size(meter, UI_GROW, 0.0f, UI_FIXED, 26.0f);
    ui_set_draw(meter, ui_draw_meter, NULL);
    ui_button(set_panel, "Back", ui_action_close, NULL);
    ui_rows_grow(set_panel);

    // The HUD: NOT modal, so it never takes input, and pushed once for the
    // whole run with the menus stacking above it. Explicit START alignment
    // because a menu's root centres what it holds and a HUD's must not.
    screen_hud = ui_screen(ui_system, "hud");
    ui_screen_set_modal(screen_hud, false);
    UIElement* hud_root = ui_screen_root(screen_hud);
    hud_root->align_main = UI_ALIGN_START;
    hud_root->align_cross = UI_ALIGN_START;
    UIElement* hud_panel = ui_panel(hud_root);
    hud_speed_label = ui_label(hud_panel, "speed  0.00");
    hud_anim_label = ui_label(hud_panel, "blend  0.00");
    ui_push(ui_system, screen_hud);

    // The menus are closed by default, the way a game's are; --ui-screen opens
    // one at startup so a headless run can photograph it.
    if (ui_screen_at_start) {
        // Looked up BY NAME through the layer rather than matched against a
        // chain of handles here: a screen already carries the name it was made
        // with, so a fifth one is reachable without editing this. The HUD is
        // pushed above, so naming it is not an error, just nothing to do.
        UIScreen* start = ui_find_screen(ui_system, ui_screen_at_start);
        if (!start)
            fprintf(stderr, "ui-screen: no screen named '%s'\n", ui_screen_at_start);
        else if (start != screen_hud)
            ui_push(ui_system, start);
    }

    ui_attach(ui_system, engine);
    return true;
}

static void ui_shutdown(void) {
    free_ui_system(ui_system);
    ui_system = NULL;
}

/*
 * --save-probe: the save format asserted where it is a pure function of a file
 * and a world, printed as `save <case> <label> <key> <numbers>` at %.6f so the
 * one regex the audio, anim and ui probes already share reads it too.
 *
 * It builds its own small world rather than using on_init's: a probe game has
 * no init callback, and a handful of entities makes every number below
 * something this file can state in closed form.
 */
typedef struct ProbeState {
    float scaled;
} ProbeState;

static ProbeState probe_state;

static const SaveField SAVE_PROBE_FIELDS[] = {
    SAVE_ROW(SAVE_FLOAT, "scaled", ProbeState, scaled),
};
#define SAVE_PROBE_COUNT ((int)(sizeof(SAVE_PROBE_FIELDS) / sizeof(SAVE_PROBE_FIELDS[0])))

/*
 * The case tags cannot express: "scaled" meant percent at version 1 and a unit
 * fraction at version 2. Same key, same type, different meaning -- so only a
 * migration can repair it, and this is what one looks like.
 */
static bool probe_migrate_scaled(cJSON* section) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(section, "scaled");
    if (!cJSON_IsNumber(item))
        return false;
    cJSON_SetNumberHelper(item, item->valuedouble / 100.0);
    return true;
}

static int save_probe_path(char* out, size_t cap, const char* slot) {
    if (!save_default_path(out, cap, slot)) {
        fprintf(stderr, "save-probe: could not resolve a save path\n");
        return 0;
    }
    return 1;
}

static int run_save_probe(Game* game, const char* which) {
    // A scene of its own: spawn_box builds a visual, and the node it makes has
    // to belong somewhere even though this probe never draws a frame. The
    // program comes from the engine the same way on_init gets it -- the probe
    // stands in for the app, so it should reach the app's path rather than have
    // create_box_node tolerate a missing one.
    Scene* scene = create_scene();
    game_set_scene(game, scene);
    pbr_shader = engine_get_program(game->engine, CETRA_PROGRAM_PBR);

    PhysicsConfig physics_config = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&physics_config);
    if (physics)
        game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    save_system = create_save_system(game);
    if (!save_system) {
        fprintf(stderr, "save-probe: no save system\n");
        return 1;
    }
    save_register_table(save_system, "gametest", SAVE_APP_VERSION, SAVE_APP_FIELDS, SAVE_APP_COUNT,
                        game);
    save_register_spawner(save_system, "box", spawn_box_from_save, game);

    char path[1024];
    if (!save_probe_path(path, sizeof(path), "probe"))
        return 1;
    int rc = 0;

    if (!strcmp(which, "roundtrip")) {
        box_count = 7;
        player_yaw = 1.25f;
        chaser_yaw = -0.5f;
        heart_timer = 0.25f;
        catch_cooldown = 1.5f;
        door_open_pending = true;
        door_open_velocity = -6.0f;
        player_touching_door = true;
        printf("save roundtrip wrote box_count %.6f\n", (double)box_count);
        printf("save roundtrip wrote player_yaw %.6f\n", (double)player_yaw);
        printf("save roundtrip wrote door_open_velocity %.6f\n", (double)door_open_velocity);
        if (!save_write(save_system, path)) {
            fprintf(stderr, "save-probe: write failed\n");
            return 1;
        }
        // Zeroed, not merely left alone: against values that already hold the
        // answer, a reader that stores nothing at all would pass.
        box_count = 0;
        player_yaw = 0.0f;
        chaser_yaw = 0.0f;
        heart_timer = 0.0f;
        catch_cooldown = 0.0f;
        door_open_pending = false;
        door_open_velocity = 0.0f;
        player_touching_door = false;
        const SaveLoadResult r = save_read(save_system, path);
        printf("save roundtrip read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save roundtrip read box_count %.6f\n", (double)box_count);
        printf("save roundtrip read player_yaw %.6f\n", (double)player_yaw);
        printf("save roundtrip read chaser_yaw %.6f\n", (double)chaser_yaw);
        printf("save roundtrip read heart_timer %.6f\n", (double)heart_timer);
        printf("save roundtrip read catch_cooldown %.6f\n", (double)catch_cooldown);
        printf("save roundtrip read door_open_pending %.6f\n", door_open_pending ? 1.0 : 0.0);
        printf("save roundtrip read door_open_velocity %.6f\n", (double)door_open_velocity);
        printf("save roundtrip read player_touching_door %.6f\n", player_touching_door ? 1.0 : 0.0);
    } else if (!strcmp(which, "entities")) {
        vec3 pos = {1.0f, 2.0f, 3.0f};
        vec3 color = {0.5f, 0.25f, 0.125f};
        Entity* box = spawn_box(game, "probe_box", pos, 0.5f, color);
        if (!box) {
            fprintf(stderr, "save-probe: could not build a box\n");
            return 1;
        }
        RigidBody* rb = entity_get_rigid_body(box);
        rigid_body_set_linear_velocity(rb, (vec3){4.0f, -5.0f, 6.0f});
        printf("save entities wrote position %.6f %.6f %.6f\n", (double)box->position[0],
               (double)box->position[1], (double)box->position[2]);
        printf("save entities wrote velocity %.6f %.6f %.6f\n", 4.0, -5.0, 6.0);
        if (!save_write(save_system, path))
            return 1;

        // Moved and stopped, so a reader that does nothing cannot pass.
        rigid_body_set_position(rb, (vec3){-9.0f, -9.0f, -9.0f});
        rigid_body_set_linear_velocity(rb, (vec3){0.0f, 0.0f, 0.0f});
        glm_vec3_copy((vec3){-9.0f, -9.0f, -9.0f}, box->position);

        const SaveLoadResult r = save_read(save_system, path);
        vec3 v;
        rigid_body_get_linear_velocity(rb, v);
        printf("save entities read count %.6f\n", (double)r.entities_restored);
        printf("save entities read position %.6f %.6f %.6f\n", (double)box->position[0],
               (double)box->position[1], (double)box->position[2]);
        printf("save entities read velocity %.6f %.6f %.6f\n", (double)v[0], (double)v[1],
               (double)v[2]);
    } else if (!strcmp(which, "spawned")) {
        vec3 pos = {2.0f, 8.0f, -3.0f};
        vec3 color = {0.75f, 0.5f, 0.25f};
        Entity* box = spawn_box(game, "box_0", pos, 0.875f, color);
        if (!box)
            return 1;
        save_note_spawn(save_system, "box_0", "box", box_params(pos, 0.875f, color));
        if (!save_write(save_system, path))
            return 1;

        // A world that never had it: the record is now the only thing that
        // says this crate ever existed.
        destroy_entity(em, box);
        printf("save spawned before found %.6f\n", find_entity_by_name(em, "box_0") ? 1.0 : 0.0);
        const SaveLoadResult r = save_read(save_system, path);
        const Entity* back = find_entity_by_name(em, "box_0");
        printf("save spawned after count %.6f\n", (double)r.entities_spawned);
        printf("save spawned after found %.6f\n", back ? 1.0 : 0.0);
        if (back)
            printf("save spawned after position %.6f %.6f %.6f\n", (double)back->position[0],
                   (double)back->position[1], (double)back->position[2]);
    } else if (!strcmp(which, "drops")) {
        // One record naming an entity nothing provides, one naming a spawner
        // this build does not have. Both drop; neither costs the file.
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FORMAT_VERSION);
        cJSON* section = cJSON_AddObjectToObject(root, "entities");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON* list = cJSON_AddArrayToObject(section, "list");
        cJSON* a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", "a_ghost");
        cJSON_AddItemToArray(list, a);
        cJSON* b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", "b_ghost");
        cJSON_AddStringToObject(b, "spawner", "no_such_recipe");
        cJSON_AddItemToArray(list, b);
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        printf("save drops read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save drops read missing %.6f\n", (double)r.dropped_missing_entity);
        printf("save drops read unknown_spawner %.6f\n", (double)r.dropped_unknown_spawner);
    } else if (!strcmp(which, "floor")) {
        player_yaw = 3.0f;
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FLOOR - 1);
        cJSON* section = cJSON_AddObjectToObject(root, "gametest");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON_AddNumberToObject(section, "player_yaw", 99.0);
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        // Refused whole: nothing below the floor is applied, so the live value
        // is the one that was already there.
        printf("save floor read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save floor read player_yaw %.6f\n", (double)player_yaw);
    } else if (!strcmp(which, "migrate")) {
        probe_state.scaled = 0.0f;
        save_register_table(save_system, "probe", 2, SAVE_PROBE_FIELDS, SAVE_PROBE_COUNT,
                            &probe_state);
        save_register_migration(save_system, "probe", 1, probe_migrate_scaled);

        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FORMAT_VERSION);
        cJSON* section = cJSON_AddObjectToObject(root, "probe");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON_AddNumberToObject(section, "scaled", 75.0); // percent, at version 1
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        printf("save migrate read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save migrate read steps %.6f\n", (double)r.migrations_run);
        printf("save migrate read scaled %.6f\n", (double)probe_state.scaled);
    } else {
        fprintf(stderr, "save-probe: unknown case '%s'\n", which);
        rc = 1;
    }

    free_save_system(save_system);
    save_system = NULL;
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
    const char* ui_probe = NULL;
    const char* save_probe = NULL;
    bool ui_enabled = true;
    // 0 = the default below. A golden states the size it was baked at, so a
    // headless app that cannot be sized can only be photographed at whatever
    // this display happens to be.
    int win_w = 0;
    int win_h = 0;
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
        } else if ((!strcmp(a, "-W") || !strcmp(a, "--width")) && i + 1 < argc) {
            win_w = atoi(argv[++i]);
        } else if ((!strcmp(a, "-H") || !strcmp(a, "--height")) && i + 1 < argc) {
            win_h = atoi(argv[++i]);
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
        } else if (!strcmp(a, "--no-chaser")) {
            no_chaser = true;
        } else if (!strcmp(a, "--puppet") && i + 1 < argc) {
            puppet_path = argv[++i];
        } else if (!strcmp(a, "--twin") && i + 1 < argc) {
            twin_clip = argv[++i];
        } else if (!strcmp(a, "--anim-probe") && i + 1 < argc) {
            anim_probe = argv[++i];
        } else if (!strcmp(a, "--ui-probe") && i + 1 < argc) {
            ui_probe = argv[++i];
        } else if (!strcmp(a, "--save-probe") && i + 1 < argc) {
            save_probe = argv[++i];
        } else if (!strcmp(a, "--no-ui")) {
            ui_enabled = false;
        } else if (!strcmp(a, "--ui-screen") && i + 1 < argc) {
            // Open a screen at startup. A menu otherwise starts closed, the way
            // a game's does, which leaves no way to photograph one: a headless
            // run has no Escape key to press.
            ui_screen_at_start = argv[++i];
        } else if (!strcmp(a, "--ui-focus") && i + 1 < argc) {
            ui_focus_steps = atoi(argv[++i]);
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

    // The settings case runs before the engine exists, because nothing it
    // measures needs one. Every other case needs a font -- FIT sizing is made
    // of measurement -- so it takes a headless game, the shape the audio and
    // anim probes already established, and still never draws a frame.
    if (ui_probe && !strcmp(ui_probe, "settings")) {
        return run_ui_probe(ui_probe);
    }
    if (ui_probe) {
        GameConfig probe_config = {.engine = {.title = "ui-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "ui-probe: could not create game\n");
            return -1;
        }
        int rc = run_ui_screens_probe(probe_game, ui_probe);
        free_game(probe_game);
        return rc;
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

    // And for the save format: a headless game, a physics world and a few
    // entities of its own, no window and no frame.
    if (save_probe) {
        GameConfig probe_config = {.engine = {.title = "save-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "save-probe: could not create game\n");
            return -1;
        }
        int rc = run_save_probe(probe_game, save_probe);
        free_game(probe_game);
        return rc;
    }

    printf("Controls (keyboard / gamepad):\n");
    printf("  WASD / left stick, dpad - Move the player (idle -> walk -> run as it speeds up)\n");
    printf("  Space / A - Jump\n");
    printf("  E / LB - Wave (the right arm, over whatever the legs are doing)\n");
    printf("A second puppet chases you. Let it catch you (--no-chaser to turn it off).\n");
    printf("  F / X - Spawn falling box\n");
    printf("  R / Y - Raycast downward from player\n");
    printf("  G / B - Print ground state\n");
    printf("  P / Start - Pause/unpause physics\n");
    printf("  Mouse drag - Orbit camera\n");
    printf("  Escape - Pause menu (--no-ui to run without any of it)\n");
    printf("Audio: a beep on jump and spawn, footsteps in time with the stride, a looping\n");
    printf("       tone at the door (--mute to silence)\n");
    printf("\nWalk into the door (right side) to push it open!\n\n");

    srand(42); // Deterministic random for testing

    // Create game
    GameConfig config = {.engine = {.title = "Physics Test - JoltC Integration",
                                    .width = win_w > 0 ? win_w : 1280,
                                    .height = win_h > 0 ? win_h : 720,
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
    if (ui_enabled && !ui_install(game->engine)) {
        free_game(game);
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
    // Unconditional: the hook carries the quicksave edges as well as the menu,
    // and it returns early of its own accord when there is no UI.
    game_set_frame_input(game, on_frame_input);
    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_pre_render(game, on_pre_render);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    // Run the game
    game_run(game);

    // Cleanup
    ui_shutdown();
    free_game(game);

    printf("Goodbye!\n");
    return 0;
}
