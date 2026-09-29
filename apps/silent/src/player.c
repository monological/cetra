#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/engine.h"
#include "cetra/game/character.h"

#include "player.h"

// The capsule: 1.7 m tall, 0.3 across the shoulders' half. Its origin is its
// centre, so it stands CAPSULE_REST above the floor.
#define CAPSULE_RADIUS      0.28f
#define CAPSULE_HALF_HEIGHT 0.57f
#define CAPSULE_REST        (CAPSULE_HALF_HEIGHT + CAPSULE_RADIUS)

// Walking pace indoors, and a jog. Slow on purpose: the house is small, and
// rushing through it spends it.
#define WALK_SPEED   1.6f
#define SPRINT_SPEED 3.4f
#define GRAVITY      9.81f

#define PITCH_LIMIT     1.45f
#define MOUSE_LOOK_RATE 0.0022f // radians per pixel

const InputAction PLAYER_ACTIONS[] = {
    {"move_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_AXIS(LEFT_X, 1), INPUT_PAD(DPAD_RIGHT, 1),
      INPUT_PAD(DPAD_LEFT, -1)}},
    {"move_y",
     {INPUT_KEY(W, 1), INPUT_KEY(S, -1), INPUT_AXIS(LEFT_Y, -1), INPUT_PAD(DPAD_UP, 1),
      INPUT_PAD(DPAD_DOWN, -1)}},
    {"sprint", {INPUT_KEY(LEFT_SHIFT, 1), INPUT_PAD(LEFT_BUMPER, 1)}},
    {"look_x", {INPUT_AXIS(RIGHT_X, 1), INPUT_KEY(RIGHT, 1), INPUT_KEY(LEFT, -1)}},
    {"look_y", {INPUT_AXIS(RIGHT_Y, -1), INPUT_KEY(UP, 1), INPUT_KEY(DOWN, -1)}},
    {"flashlight", {INPUT_KEY(F, 1), INPUT_PAD(Y, 1)}},
    {"release_cursor", {INPUT_KEY(TAB, 1)}},
};
const int PLAYER_ACTION_COUNT = (int)(sizeof(PLAYER_ACTIONS) / sizeof(PLAYER_ACTIONS[0]));

// Raw motion where the platform has it, so the desktop's acceleration curve
// stays out of the look. Nothing captures headless: there is no pointer.
static void set_cursor_captured(Player* p, Engine* engine, bool captured) {
    if (engine->headless || captured == p->cursor_captured)
        return;
    glfwSetInputMode(engine->window, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(engine->window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    p->cursor_captured = captured;
    p->skip_first_delta = captured;
}

void player_init(Player* p, Game* game, PhysicsWorld* physics, EntityManager* em, const vec3 feet,
                 float yaw) {
    *p = (Player){0};
    p->entity = create_entity(em, "player");
    glm_vec3_copy((vec3){feet[0], feet[1] + CAPSULE_REST + 0.02f, feet[2]}, p->entity->position);
    CharacterControllerConfig cc = character_controller_default_config();
    cc.capsule_radius = CAPSULE_RADIUS;
    cc.capsule_half_height = CAPSULE_HALF_HEIGHT;
    // Porch steps are 15 cm; a kerb is 15. Nothing in the world asks for more.
    cc.step_height = 0.25f;
    cc.max_slope_angle = 45.0f;
    entity_add_character_controller(p->entity, physics, &cc);

    p->rig = create_camera_rig();
    p->rig->steers_controls = true;
    p->rig->look_lift = PLAYER_EYE_HEIGHT - CAPSULE_REST;
    p->rig->pitch_min = -PITCH_LIMIT;
    p->rig->pitch_max = PITCH_LIMIT;
    glm_vec3_copy(p->entity->position, p->rig->anchor);
    camera_rig_set_distance(p->rig, 0.0f);
    camera_rig_aim(p->rig, yaw, 0.0f);
    engine_set_camera_rig(game->engine, p->rig);
}

void player_update(Player* p, Game* game, double dt) {
    CharacterController* cc = p->entity ? entity_get_character_controller(p->entity) : NULL;
    if (!cc)
        return;
    vec3 input_dir;
    input_action_move(&game->input, "move_x", "move_y", input_dir);

    float yaw = 0.0f;
    camera_rig_move_basis(p->rig, &yaw);
    vec3 fwd = {sinf(yaw), 0.0f, cosf(yaw)};
    vec3 right = {-cosf(yaw), 0.0f, sinf(yaw)};
    const float speed = input_action_down(&game->input, "sprint") ? SPRINT_SPEED : WALK_SPEED;

    vec3 vel;
    character_controller_get_velocity(cc, vel);
    vec3 move = {0.0f, 0.0f, 0.0f};
    glm_vec3_muladds(fwd, -input_dir[2] * speed, move);
    glm_vec3_muladds(right, input_dir[0] * speed, move);
    // SET, not accumulated, so letting go stops. Zero vertical on the ground:
    // any residual downward speed is resolved along a slope as a slide.
    vel[0] = move[0];
    vel[2] = move[2];
    if (character_controller_is_grounded(cc))
        vel[1] = 0.0f;
    else
        vel[1] -= GRAVITY * (float)dt;
    character_controller_set_velocity(cc, vel);
}

void player_pre_render(Player* p, Game* game, const vec3* pin_eye, const vec3* pin_target) {
    Engine* engine = game->engine;
    if (pin_eye && pin_target) {
        camera_rig_set_pose(p->rig, *pin_eye, *pin_target);
        glm_vec3_copy((float*)*pin_eye, p->eye);
        return;
    }
    if (p->cursor_captured && p->skip_first_delta) {
        p->skip_first_delta = false;
    } else if (p->cursor_captured) {
        double dx = 0.0, dy = 0.0;
        input_mouse_delta(&game->input, &dx, &dy);
        camera_rig_aim(p->rig, p->rig->yaw - (float)dx * MOUSE_LOOK_RATE,
                       p->rig->pitch - (float)dy * MOUSE_LOOK_RATE);
    }
    if (!p->cursor_captured && input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT) &&
        !engine_gui_wants_mouse())
        set_cursor_captured(p, engine, true);
    if (input_action_pressed(&game->input, "release_cursor"))
        set_cursor_captured(p, engine, false);

    glm_vec3_copy(p->entity->position, p->rig->anchor);
    // At distance zero the eye IS the aim point, so it is known here, before
    // the engine applies the rig -- which is what lets a light ride the head
    // in the same frame rather than one behind it.
    glm_vec3_copy(p->rig->anchor, p->eye);
    p->eye[1] += p->rig->look_lift;
    camera_rig_update(p->rig, (float)game->sim_clock.delta,
                      input_action_value(&game->input, "look_x"),
                      input_action_value(&game->input, "look_y"));
}

void player_eye(const Player* p, vec3 eye, vec3 forward) {
    glm_vec3_copy((float*)p->eye, eye);
    camera_rig_direction(p->rig, forward);
}
