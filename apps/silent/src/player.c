#include <float.h>
#include <math.h>
#include <stdio.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/animation.h"
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

// A crouch (spec 13.48): a capsule 1.15 m tall with the eye at 1.05, taken at half a walk. People
// crouch-walking keep the eye 1.0 to 1.2 m up and go 0.6 to 0.9 m/s.
#define CROUCH_HALF_HEIGHT 0.295f
#define CROUCH_EYE_HEIGHT  1.05f
#define CROUCH_SPEED       0.8f
// Seconds the eye takes to go down, and to come back up. Going down it stands for a moment over
// the shortened capsule, a radius behind its front: walking, braking to a crouch's pace covers
// less than that radius in the time, so the eye never reaches a beam the capsule has gone under.
#define CROUCH_DOWN 0.2f
#define CROUCH_UP   0.3f
#define CROUCH_BOB  0.5f // the share of the head's bob left at a full crouch

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

// The body where its entity stands, still, looking along `yaw` and `pitch`. A window's first frame
// may run no step, and is drawn from here.
static void stand(Player* p, float yaw, float pitch) {
    glm_vec3_copy(p->entity->position, p->step_from);
    glm_vec3_copy(p->entity->position, p->view);
    p->speed = 0.0f;
    p->heading = yaw;
    camera_rig_aim(p->rig, yaw, pitch);
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
    p->interpolate = true;
    p->gait = create_gait();
    p->use_gait = true;

    p->rig = create_camera_rig();
    p->rig->steers_controls = true;
    p->rig->look_lift = player_eye_height(p) - CAPSULE_REST;
    p->rig->pitch_min = -PITCH_LIMIT;
    p->rig->pitch_max = PITCH_LIMIT;
    glm_vec3_copy(p->entity->position, p->rig->anchor);
    camera_rig_set_distance(p->rig, 0.0f);
    stand(p, yaw, pitch);
    engine_set_camera_rig(game->engine, p->rig);
}

// Moves `from` toward `to` by at most `step`.
static float approach(float from, float to, float step) {
    return from < to ? fminf(from + step, to) : fmaxf(from - step, to);
}

// The horizontal velocity asked of the body this step: its heading turned toward what the stick
// asks, and its speed ramped toward it -- or toward a stop, when asked round past REVERSE.
static void ramp_move(Player* p, const vec3 want, bool grounded, float dt) {
    const float control = grounded ? 1.0f : AIR_CONTROL;
    const float asked = hypotf(want[0], want[2]);
    float target = 0.0f;
    if (asked > 1e-4f) {
        const float to = atan2f(want[0], want[2]);
        if (p->speed <= 1e-4f)
            p->heading = to; // standing, it sets off the way it is asked
        const float turn = wrap_pi(to - p->heading);
        if (fabsf(turn) <= REVERSE) {
            const float most = TURN_RATE * control * dt;
            p->heading = wrap_pi(p->heading + glm_clamp(turn, -most, most));
            target = asked;
        }
    }
    p->speed = approach(p->speed, target, (target > p->speed ? ACCEL : BRAKE) * control * dt);
}

// The capsule is a crouch's: the engine keeps the height it has.
static bool crouched(const CharacterController* cc) {
    return cc->half_height < CAPSULE_HALF_HEIGHT;
}

// Fixed step: the capsule a crouch's while the toggle or the held key asks, a standing body's
// otherwise. Standing is refused under anything lower than a standing body, and asked again every
// step until there is room. While a screen holds the game's input the stance is kept, or a held
// crouch would stand up behind it.
static void crouch_step(const Player* p, Game* game, CharacterController* cc) {
    if (input_is_suppressed(&game->input))
        return;
    const bool want = p->crouch_toggled || input_action_down(&game->input, "crouch");
    character_controller_set_half_height(cc, want ? CROUCH_HALF_HEIGHT : CAPSULE_HALF_HEIGHT);
}

// Per frame: the crouch keys, and the eye following the capsule down and up. A press is read once
// a frame, since read in a fixed step it would count twice on a frame of two steps and not at all
// on a frame of none; sprinting asks to stand.
static void crouch_frame(Player* p, Game* game, const CharacterController* cc, float dt) {
    if (input_action_pressed(&game->input, "crouch_toggle"))
        p->crouch_toggled = !p->crouch_toggled;
    if (input_action_pressed(&game->input, "sprint"))
        p->crouch_toggled = false;
    const bool low = cc && crouched(cc);
    p->crouch_eased =
        approach(p->crouch_eased, low ? 1.0f : 0.0f, dt / (low ? CROUCH_DOWN : CROUCH_UP));
    p->rig->look_lift = player_eye_height(p) - CAPSULE_REST;
    // Lagging a descent, the eye goes no nearer the capsule's top than the near plane reaches, or
    // under a ceiling the capsule only just clears the view would cut into it. The capsule's
    // height is its top over the feet, since the engine keeps its foot where it was.
    const float tall = 2.0f * ((cc ? cc->half_height : CAPSULE_HALF_HEIGHT) + CAPSULE_RADIUS);
    const Camera* cam = game->engine->camera;
    p->gait->lag_above = fmaxf(tall - player_eye_height(p) - (cam ? cam->near_clip : 0.0f), 0.0f);
}

// One line a fixed step: where the feet are, what the step moved, and what the gait made of it.
static void trace_step(const Player* p, const Game* game, const CharacterController* cc,
                       const vec3 moved) {
    const float* at = p->entity->position;
    printf("gait step %5ld feet %8.4f %8.4f %8.4f dy %+.4f dh %.4f ground %d up %.3f speed %.3f "
           "grade %+.3f stairs %.2f pace %.3f crouched %d\n",
           lround(game->time / game->fixed_timestep), (double)at[0], (double)(at[1] - CAPSULE_REST),
           (double)at[2], (double)moved[1], (double)hypotf(moved[0], moved[2]),
           (int)cc->ground_state, (double)cc->ground_normal[1], (double)p->speed,
           (double)p->gait->grade, (double)p->gait->stairs, (double)p->gait->pace,
           (int)crouched(cc));
}

// One line a frame, and one more for a foot that came down: the feet as drawn, the eye the walk
// puts them under (which a pin overrides), the stride, and the bob the eye took.
static void trace_frame(const Player* p, const Game* game, const vec3 eye, const vec3 bob) {
    const size_t frame = game->engine->total_frames;
    const Gait* g = p->gait;
    if (g->footfall.kind != GAIT_FALL_NONE)
        printf("gait fall frame %5zu %s %s speed %.3f\n", frame, gait_fall_name(g->footfall.kind),
               g->footfall.foot ? "right" : "left", (double)g->footfall.speed);
    printf("gait frame %5zu feet %.4f eye %.4f phase %.3f speed %.3f bob %+.4f %+.4f %+.4f "
           "crouch %.3f\n",
           frame, (double)(p->view[1] - CAPSULE_REST), (double)eye[1], (double)g->phase,
           (double)g->speed, (double)bob[0], (double)bob[1], (double)bob[2],
           (double)p->crouch_eased);
}

void player_update(Player* p, Game* game, double dt) {
    CharacterController* cc = p->entity ? entity_get_character_controller(p->entity) : NULL;
    if (!cc)
        return;
    crouch_step(p, game, cc);
    vec3 input_dir;
    input_action_move(&game->input, "move_x", "move_y", input_dir);
    float yaw = 0.0f;
    camera_rig_move_basis(p->rig, &yaw);
    vec3 fwd = {sinf(yaw), 0.0f, cosf(yaw)};
    vec3 right = {-cosf(yaw), 0.0f, sinf(yaw)};
    const float speed = crouched(cc)                                ? CROUCH_SPEED
                        : input_action_down(&game->input, "sprint") ? SPRINT_SPEED
                                                                    : WALK_SPEED;
    vec3 want = {0.0f, 0.0f, 0.0f};
    glm_vec3_muladds(fwd, -input_dir[2] * speed, want);
    glm_vec3_muladds(right, input_dir[0] * speed, want);

    // Nothing has moved the capsule yet this step, so its position is where the last step left
    // it, and the move from step_from is the last step's whole.
    const bool grounded = character_controller_is_grounded(cc);
    vec3 moved;
    glm_vec3_sub(p->entity->position, p->step_from, moved);
    gait_step(p->gait, moved, grounded, hypotf(want[0], want[2]), (float)dt);
    if (p->trace)
        trace_step(p, game, cc, moved);
    glm_vec3_copy(p->entity->position, p->step_from);

    // A climb is taken at the pace a person keeps on it. Jolt slides the capsule up a slope at the
    // speed asked times the slope's cosine, which leaves a run of the speed asked times the
    // cosine squared, 1 / (1 + grade^2); asked for that much more, the run is the pace's.
    if (p->use_gait) {
        const float up = fmaxf(p->gait->grade, 0.0f);
        glm_vec3_scale(want, p->gait->pace * (1.0f + up * up), want);
    }
    ramp_move(p, want, grounded, (float)dt);
    // The ramp is kept here rather than read back, since Jolt cancels what pushes into steep
    // ground. Zero vertical on the ground: any residual downward speed is resolved along a slope
    // as a slide.
    vec3 vel;
    character_controller_get_velocity(cc, vel);
    vel[0] = p->speed * sinf(p->heading);
    vel[2] = p->speed * cosf(p->heading);
    if (grounded)
        vel[1] = 0.0f;
    else
        vel[1] -= GRAVITY * (float)dt;
    character_controller_set_velocity(cc, vel);
}

void player_pre_render(Player* p, Game* game, const vec3* pin_eye, const vec3* pin_target) {
    Engine* engine = game->engine;
    const float dt = (float)game->frame_clock.delta;
    // The body drawn between the last two steps, alpha of the way from the earlier: at most one
    // step behind, and never jumping a step at a time.
    const double alpha = p->interpolate ? game->accumulator / game->fixed_timestep : 1.0;
    vec3 view, moved;
    glm_vec3_lerp(p->step_from, p->entity->position, (float)alpha, view);
    glm_vec3_sub(view, p->view, moved);
    glm_vec3_copy(view, p->view);
    CharacterController* cc = entity_get_character_controller(p->entity);
    crouch_frame(p, game, cc, dt);
    // The stride goes on under a pinned camera, where its footsteps are still heard.
    gait_frame(p->gait, moved, cc && character_controller_is_grounded(cc), p->rig->yaw, dt);
    // The eye the walk puts: the body's, lagging the ground and over the feet, which bob less
    // bent low.
    vec3 anchor, bob = {0.0f, 0.0f, 0.0f};
    glm_vec3_copy(p->view, anchor);
    if (p->use_gait) {
        anchor[1] += p->gait->smooth_offset;
        glm_vec3_scale(p->gait->bob_offset, glm_smoothinterp(1.0f, CROUCH_BOB, p->crouch_eased),
                       bob);
        glm_vec3_add(anchor, bob, anchor);
    }
    if (p->trace)
        trace_frame(p, game, (vec3){anchor[0], anchor[1] + p->rig->look_lift, anchor[2]}, bob);
    // What a hand reaches from: the body's own eye, which no lag or bob moves.
    glm_vec3_copy(p->view, p->reach_from);
    p->reach_from[1] += p->rig->look_lift;
    if (pin_eye && pin_target) {
        camera_rig_set_pose(p->rig, *pin_eye, *pin_target);
        glm_vec3_copy((float*)*pin_eye, p->reach_from);
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
    if (engine->show_gui)
        p->cursor_wanted = false;
    else if (input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT) &&
             !engine_gui_wants_mouse() && !held)
        p->cursor_wanted = true;
    set_cursor_captured(p, engine, p->cursor_wanted && !held);

    glm_vec3_copy(anchor, p->rig->anchor);
    camera_rig_update(p->rig, dt, input_action_value(&game->input, "look_x"),
                      input_action_value(&game->input, "look_y"));
}

void player_free(Player* p) {
    free_gait(p->gait);
    p->gait = NULL;
}

void player_restore(Player* p) {
    CharacterController* cc = p->entity ? entity_get_character_controller(p->entity) : NULL;
    if (!cc)
        return;
    character_controller_get_position(cc, p->entity->position);
    character_controller_set_velocity(cc, GLM_VEC3_ZERO);
    stand(p, p->rig->yaw, p->rig->pitch);
    gait_reset(p->gait);
    // A click takes the cursor, as at the start.
    p->cursor_wanted = false;
    // Down always fits; up is checked against what is over the head where it now stands.
    p->crouch_toggled = false;
    character_controller_set_half_height(cc, CROUCH_HALF_HEIGHT);
    const bool stood = character_controller_set_half_height(cc, CAPSULE_HALF_HEIGHT);
    p->crouch_eased = stood ? 0.0f : 1.0f;
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

float player_eye_height(const Player* p) {
    return glm_smoothinterp(PLAYER_EYE_HEIGHT, CROUCH_EYE_HEIGHT, p->crouch_eased);
}

bool player_crouched(const Player* p) {
    const CharacterController* cc = entity_get_character_controller(p->entity);
    return cc && crouched(cc);
}

float player_reach_distance(const Player* p, const vec3 at) {
    vec3 forward = {0.0f, 0.0f, 0.0f}, to = {0.0f, 0.0f, 0.0f};
    camera_rig_direction(p->rig, forward);
    glm_vec3_sub((float*)at, (float*)p->reach_from, to);
    const float dist = glm_vec3_norm(to);
    if (dist > REACH || dist < 1e-4f || glm_vec3_dot(to, forward) / dist < cosf(REACH_CONE))
        return FLT_MAX;
    return dist;
}
