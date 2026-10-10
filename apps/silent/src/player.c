#include <float.h>
#include <math.h>
#include <stdio.h>

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

// Getting going and stopping (spec 13.47): the speed ramps, quicker down than up, while the way it
// goes turns fast, so a turn of the head does not skid like ice.
#define ACCEL       5.0f  // m/s^2: a walk is reached in a third of a second
#define BRAKE       8.0f  // m/s^2
#define TURN_RATE   10.0f // rad/s the way it goes turns toward the way it is asked to
#define REVERSE     1.75f // radians: asked round further than this, it stops before going back
#define AIR_CONTROL 0.2f  // the share of those it keeps with no ground under it

#define PITCH_LIMIT     1.45f
#define MOUSE_LOOK_RATE 0.0022f // radians per pixel

// What a hand goes to: within REACH metres of the eye and REACH_CONE radians of where it looks.
#define REACH      1.9f
#define REACH_CONE 0.6f

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
                 float yaw, float pitch) {
    *p = (Player){0};
    p->entity = create_entity(em, "player");
    glm_vec3_copy((vec3){feet[0], feet[1] + CAPSULE_REST + 0.02f, feet[2]}, p->entity->position);
    CharacterControllerConfig cc = character_controller_default_config();
    cc.capsule_radius = CAPSULE_RADIUS;
    cc.capsule_half_height = CAPSULE_HALF_HEIGHT;
    // Porch steps and kerbs rise 15 cm; a flight is a ramp (kit_frame_ramp), and the cellar's,
    // the steepest, is pitched at 47 degrees.
    cc.step_height = 0.25f;
    cc.max_slope_angle = glm_rad(50.0f);
    entity_add_character_controller(p->entity, physics, &cc);
    // A window's first frame may run no step, and is drawn from here.
    glm_vec3_copy(p->entity->position, p->step_from);
    glm_vec3_copy(p->entity->position, p->view);
    p->frame_at = game->time + game->accumulator;
    p->interpolate = true;

    p->rig = create_camera_rig();
    p->rig->steers_controls = true;
    p->rig->look_lift = PLAYER_EYE_HEIGHT - CAPSULE_REST;
    p->rig->pitch_min = -PITCH_LIMIT;
    p->rig->pitch_max = PITCH_LIMIT;
    glm_vec3_copy(p->entity->position, p->rig->anchor);
    camera_rig_set_distance(p->rig, 0.0f);
    camera_rig_aim(p->rig, yaw, pitch);
    engine_set_camera_rig(game->engine, p->rig);
}

// Moves `from` toward `to` by at most `step`.
static float approach(float from, float to, float step) {
    return from < to ? fminf(from + step, to) : fmaxf(from - step, to);
}

// The horizontal velocity asked for this step, ramped from the last toward what the stick asks.
static void ramp_move(Player* p, const vec3 want, bool grounded, float dt) {
    const float control = grounded ? 1.0f : AIR_CONTROL;
    const float asked = hypotf(want[0], want[2]);
    float speed = hypotf(p->move[0], p->move[1]);
    const bool moving = speed > 1e-4f;
    float heading = moving ? atan2f(p->move[0], p->move[1]) : atan2f(want[0], want[2]);
    if (asked > 1e-4f) {
        const float to = atan2f(want[0], want[2]);
        const float turn = remainderf(to - heading, 2.0f * GLM_PIf);
        if (moving && fabsf(turn) > REVERSE) {
            speed = fmaxf(speed - BRAKE * control * dt, 0.0f);
        } else {
            const float most = TURN_RATE * control * dt;
            heading = moving ? heading + glm_clamp(turn, -most, most) : to;
            speed = approach(speed, asked, (asked > speed ? ACCEL : BRAKE) * control * dt);
        }
    } else {
        speed = fmaxf(speed - BRAKE * control * dt, 0.0f);
    }
    p->move[0] = speed * sinf(heading);
    p->move[1] = speed * cosf(heading);
}

void player_update(Player* p, Game* game, double dt) {
    CharacterController* cc = p->entity ? entity_get_character_controller(p->entity) : NULL;
    if (!cc)
        return;
    // Nothing has moved the capsule yet this step, so its position is where the last step left it.
    if (p->trace) {
        const float* at = p->entity->position;
        vec3 d;
        glm_vec3_sub((float*)at, p->step_from, d);
        printf(
            "gait step %5d feet %8.4f %8.4f %8.4f dy %+.4f dh %.4f ground %d up %.3f speed %.3f\n",
            p->steps, (double)at[0], (double)(at[1] - CAPSULE_REST), (double)at[2], (double)d[1],
            (double)sqrtf(d[0] * d[0] + d[2] * d[2]), (int)cc->ground_state,
            (double)cc->ground_normal[1],
            (double)sqrtf(cc->velocity[0] * cc->velocity[0] + cc->velocity[2] * cc->velocity[2]));
    }
    p->steps++;
    glm_vec3_copy(p->entity->position, p->step_from);
    vec3 input_dir;
    input_action_move(&game->input, "move_x", "move_y", input_dir);

    float yaw = 0.0f;
    camera_rig_move_basis(p->rig, &yaw);
    vec3 fwd = {sinf(yaw), 0.0f, cosf(yaw)};
    vec3 right = {-cosf(yaw), 0.0f, sinf(yaw)};
    const float speed = input_action_down(&game->input, "sprint") ? SPRINT_SPEED : WALK_SPEED;

    vec3 want = {0.0f, 0.0f, 0.0f};
    glm_vec3_muladds(fwd, -input_dir[2] * speed, want);
    glm_vec3_muladds(right, input_dir[0] * speed, want);
    const bool grounded = character_controller_is_grounded(cc);
    ramp_move(p, want, grounded, (float)dt);
    // The ramp is kept here rather than read back, since Jolt cancels what pushes into steep
    // ground. Zero vertical on the ground: any residual downward speed is resolved along a slope
    // as a slide.
    vec3 vel;
    character_controller_get_velocity(cc, vel);
    vel[0] = p->move[0];
    vel[2] = p->move[1];
    if (grounded)
        vel[1] = 0.0f;
    else
        vel[1] -= GRAVITY * (float)dt;
    character_controller_set_velocity(cc, vel);
}

void player_pre_render(Player* p, Game* game, double alpha, const vec3* pin_eye,
                       const vec3* pin_target) {
    Engine* engine = game->engine;
    // The render clock: whole steps run plus the part of one the accumulator holds. It moves every
    // frame, where the sim clock's delta is 0 on a frame that ran no step.
    const double now = game->time + game->accumulator;
    p->frame_dt = (float)fmax(now - p->frame_at, 0.0);
    p->frame_at = now;
    // The body drawn between the last two steps, alpha of the way from the earlier: at most one
    // step behind, and never jumping a step at a time.
    if (p->interpolate)
        glm_vec3_lerp(p->step_from, p->entity->position, (float)alpha, p->view);
    else
        glm_vec3_copy(p->entity->position, p->view);
    if (pin_eye && pin_target) {
        camera_rig_set_pose(p->rig, *pin_eye, *pin_target);
        return;
    }
    const bool held = input_is_suppressed(&game->input);
    if (p->cursor_captured && p->skip_first_delta) {
        p->skip_first_delta = false;
    } else if (p->cursor_captured && !held) {
        double dx = 0.0, dy = 0.0;
        input_mouse_delta(&game->input, &dx, &dy);
        camera_rig_aim(p->rig, p->rig->yaw - (float)dx * MOUSE_LOOK_RATE,
                       p->rig->pitch - (float)dy * MOUSE_LOOK_RATE);
    }
    // The debug GUI wants the pointer: while it is open the cursor is free and a click does not
    // take it back, or its sliders could not be reached. The arrow keys still look. While the
    // game's input is held a click takes nothing either: nobody looks round a room they cannot see.
    // Captured after the look, so the jump a capture makes lands in the delta it skips.
    if (engine->show_gui || input_action_pressed(&game->input, "release_cursor"))
        p->cursor_wanted = false;
    else if (input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT) &&
             !engine_gui_wants_mouse() && !held)
        p->cursor_wanted = true;
    set_cursor_captured(p, engine, p->cursor_wanted && !held);

    glm_vec3_copy(p->view, p->rig->anchor);
    camera_rig_update(p->rig, p->frame_dt, input_action_value(&game->input, "look_x"),
                      input_action_value(&game->input, "look_y"));
}

void player_feet(const Player* p, vec3 out) {
    glm_vec3_copy(p->entity->position, out);
    out[1] -= CAPSULE_REST;
}

void player_view_feet(const Player* p, vec3 out) {
    glm_vec3_copy((float*)p->view, out);
    out[1] -= CAPSULE_REST;
}

void player_velocity(const Player* p, vec3 out) {
    character_controller_get_velocity(entity_get_character_controller(p->entity), out);
}

void player_eye(const Player* p, vec3 eye, vec3 forward) {
    // The rig's pose is this frame's once player_pre_render has run, before the
    // engine applies it -- which is what lets a light ride the head in the same
    // frame rather than one behind it.
    glm_vec3_copy((float*)p->rig->pose.eye, eye);
    camera_rig_direction(p->rig, forward);
}

float player_reach_distance(const Player* p, const vec3 at) {
    vec3 eye = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, 0.0f}, to = {0.0f, 0.0f, 0.0f};
    player_eye(p, eye, forward);
    glm_vec3_sub((float*)at, eye, to);
    const float dist = glm_vec3_norm(to);
    if (dist > REACH || dist < 1e-4f || glm_vec3_dot(to, forward) / dist < cosf(REACH_CONE))
        return FLT_MAX;
    return dist;
}
