#include <float.h>
#include <math.h>
#include <stdio.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/animation.h"
#include "cetra/engine.h"
#include "cetra/game/character.h"
#include "cetra/loading_screen.h"

#include "kit.h"
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

// The window's cursor: DISABLED while the mouse looks, with raw motion where the platform has it
// so the desktop's acceleration curve stays out of the look, NORMAL to point, HIDDEN otherwise.
// A change moves the cursor -- a capture centres it, a release puts it back -- so the delta after
// one is not a movement. Nothing is set headless: there is no pointer.
static void set_cursor_mode(Player* p, Engine* engine, int mode) {
    if (engine->headless || mode == p->cursor_mode)
        return;
    const bool captured = mode == GLFW_CURSOR_DISABLED;
    glfwSetInputMode(engine->window, GLFW_CURSOR, mode);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(engine->window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    p->cursor_mode = mode;
    p->cursor_captured = captured;
    p->skip_first_delta = true;
}

// What drives a screen from the keys or a pad.
static const char* const SCREEN_ACTIONS[] = {"menu_up",    "menu_down",   "menu_left",
                                             "menu_right", "menu_accept", "menu_back",
                                             "map_pan_x",  "map_pan_y",   "map_zoom"};

// Whether the mouse is what drives the screen over the game: moved, clicked or scrolled since one
// of the screen's keys or a pad last did.
static bool pointing(Player* p, const GameInputState* input) {
    double dx = 0.0, dy = 0.0, sx = 0.0, sy = 0.0;
    input_mouse_delta(input, &dx, &dy);
    input_scroll(input, &sx, &sy);
    if (p->skip_first_delta)
        p->skip_first_delta = false;
    else if (dx != 0.0 || dy != 0.0)
        p->pointing = true;
    if (sx != 0.0 || sy != 0.0 || input_mouse_pressed(input, GLFW_MOUSE_BUTTON_LEFT) ||
        input_mouse_pressed(input, GLFW_MOUSE_BUTTON_RIGHT))
        p->pointing = true;
    for (size_t i = 0; i < sizeof(SCREEN_ACTIONS) / sizeof(SCREEN_ACTIONS[0]); i++)
        if (input_action_value(input, SCREEN_ACTIONS[i]) != 0.0f)
            p->pointing = false;
    return p->pointing;
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
    p->speed = kit_approach(p->speed, target, (target > p->speed ? ACCEL : BRAKE) * control * dt);
}

// Standing on something: Jolt's "on ground". Steep ground is what the capsule slides down or only
// leans on, and gravity goes on acting there, as in Jolt's own sample; counted as standing, a wall
// brushed in mid-air held the body up and it walked straight out over the cellar flight.
static bool standing(const CharacterController* cc) {
    return cc && character_controller_get_ground_state(cc) == CHAR_GROUND_ON_GROUND;
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
    if (input_is_suppressed(&game->input) || p->holding)
        return;
    const bool want = p->crouch_toggled || input_action_down(&game->input, "crouch");
    character_controller_set_half_height(cc, want ? CROUCH_HALF_HEIGHT : CAPSULE_HALF_HEIGHT);
}

// Per frame: the crouch keys, and the eye following the capsule down and up. A press is read once
// a frame, since read in a fixed step it would count twice on a frame of two steps and not at all
// on a frame of none; sprinting asks to stand.
static void crouch_frame(Player* p, Game* game, const CharacterController* cc, float dt) {
    if (input_action_pressed(&game->input, "crouch_toggle") && !p->holding)
        p->crouch_toggled = !p->crouch_toggled;
    if (input_action_pressed(&game->input, "sprint") && !p->holding)
        p->crouch_toggled = false;
    const bool low = cc && crouched(cc);
    p->crouch_eased =
        kit_approach(p->crouch_eased, low ? 1.0f : 0.0f, dt / (low ? CROUCH_DOWN : CROUCH_UP));
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
    // Riding, the boat carries the body after this, so this step's move is from here.
    if (p->riding) {
        glm_vec3_copy(p->entity->position, p->step_from);
        return;
    }
    crouch_step(p, game, cc);
    vec3 input_dir = {0.0f, 0.0f, 0.0f};
    if (!p->holding)
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
    const bool grounded = standing(cc);
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
    const CharacterController* cc = entity_get_character_controller(p->entity);
    // The body's own eye less the rig's lift, which no lag or bob moves: sat in a boat, the seat's.
    vec3 body, anchor, bob = {0.0f, 0.0f, 0.0f};
    glm_vec3_copy(p->view, body);
    if (p->riding)
        glm_vec3_sub(p->seat_eye, (vec3){0.0f, p->rig->look_lift, 0.0f}, body);
    glm_vec3_copy(body, anchor);
    if (!p->riding) {
        crouch_frame(p, game, cc, dt);
        // The stride goes on under a pinned camera, where its footsteps are still heard.
        gait_frame(p->gait, moved, standing(cc), p->rig->yaw, dt);
        // The eye the walk puts: lagging the ground and over the feet, which bob less bent low.
        if (p->use_gait) {
            anchor[1] += p->gait->smooth_offset;
            glm_vec3_scale(p->gait->bob_offset, glm_smoothinterp(1.0f, CROUCH_BOB, p->crouch_eased),
                           bob);
            glm_vec3_add(anchor, bob, anchor);
        }
    }
    if (p->trace)
        trace_frame(p, game, (vec3){anchor[0], anchor[1] + p->rig->look_lift, anchor[2]}, bob);
    // What a hand reaches from.
    glm_vec3_add(body, (vec3){0.0f, p->rig->look_lift, 0.0f}, p->reach_from);
    if (pin_eye && pin_target) {
        camera_rig_set_pose(p->rig, *pin_eye, *pin_target);
        glm_vec3_copy((float*)*pin_eye, p->reach_from);
        return;
    }
    const bool held = input_is_suppressed(&game->input);
    if (p->cursor_captured && p->skip_first_delta) {
        p->skip_first_delta = false;
    } else if (p->cursor_captured && !held && !p->holding) {
        double dx = 0.0, dy = 0.0;
        input_mouse_delta(&game->input, &dx, &dy);
        camera_rig_aim(p->rig, p->rig->yaw - (float)dx * MOUSE_LOOK_RATE,
                       p->rig->pitch - (float)dy * MOUSE_LOOK_RATE);
    }
    // The debug GUI wants the pointer: while it is open the cursor is shown and a click does not
    // take it, or its sliders could not be reached. The arrow keys still look. While the game's
    // input is held a click takes nothing either: nobody looks round a room they cannot see.
    // A window shares the desktop, so there the cursor waits for a click; fullscreen has nothing
    // else on the screen to point at, and takes it whenever play has the input. A screen over the
    // game opens with the cursor hidden, as the key that opened it leaves it, and shows it once
    // the mouse moves; the loading screen takes any press, a click included, and points at nothing.
    // Set after the look, so the jump a change makes lands in the delta it skips.
    int mode = GLFW_CURSOR_NORMAL;
    if (engine->show_gui) {
        p->cursor_wanted = false;
    } else if (!held) {
        if (input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT) && !engine_gui_wants_mouse())
            p->cursor_wanted = true;
        if (p->cursor_wanted || engine->window_mode == ENGINE_WINDOW_FULLSCREEN)
            mode = GLFW_CURSOR_DISABLED;
        p->pointing = false;
    } else if (engine_loading_screen_shown(engine) || !pointing(p, &game->input)) {
        mode = GLFW_CURSOR_HIDDEN;
    }
    set_cursor_mode(p, engine, mode);

    glm_vec3_copy(anchor, p->rig->anchor);
    const float look = p->holding ? 0.0f : 1.0f;
    camera_rig_update(p->rig, dt, look * input_action_value(&game->input, "look_x"),
                      look * input_action_value(&game->input, "look_y"));
}

void player_free(Player* p) {
    free_gait(p->gait);
    p->gait = NULL;
}

// The crouch let go, and the capsule stood up where it is if there is room over its head, crouched
// if not: down always fits, and up is checked.
static void stand_up(Player* p, CharacterController* cc) {
    p->crouch_toggled = false;
    character_controller_set_half_height(cc, CROUCH_HALF_HEIGHT);
    const bool stood = character_controller_set_half_height(cc, CAPSULE_HALF_HEIGHT);
    p->crouch_eased = stood ? 0.0f : 1.0f;
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
    stand_up(p, cc);
}

void player_board(Player* p, float seconds) {
    CharacterController* cc = entity_get_character_controller(p->entity);
    if (!cc)
        return;
    // Stood up first, so the body gets out of the boat a standing one.
    stand_up(p, cc);
    p->rig->look_lift = player_eye_height(p) - CAPSULE_REST;
    character_controller_set_velocity(cc, GLM_VEC3_ZERO);
    cc->enabled = false;
    p->speed = 0.0f;
    p->riding = true;
    gait_reset(p->gait);
    camera_rig_blend_from_here(p->rig, seconds);
}

void player_carry(Player* p, const vec3 feet) {
    CharacterController* cc = entity_get_character_controller(p->entity);
    glm_vec3_copy((vec3){feet[0], feet[1] + CAPSULE_REST, feet[2]}, p->entity->position);
    if (cc)
        character_controller_set_position(cc, p->entity->position);
}

void player_seat(Player* p, const vec3 eye, float turn) {
    glm_vec3_copy((float*)eye, p->seat_eye);
    if (turn != 0.0f)
        camera_rig_aim(p->rig, p->rig->yaw + turn, p->rig->pitch);
}

void player_disembark(Player* p, const vec3 feet, float yaw, float seconds) {
    CharacterController* cc = entity_get_character_controller(p->entity);
    if (!cc)
        return;
    camera_rig_blend_from_here(p->rig, seconds);
    player_carry(p, feet);
    character_controller_set_velocity(cc, GLM_VEC3_ZERO);
    cc->enabled = true;
    p->riding = false;
    stand(p, yaw, p->rig->pitch);
    gait_reset(p->gait);
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

float player_reach_within(const Player* p, const vec3 at, float reach, float cone) {
    vec3 forward = {0.0f, 0.0f, 0.0f}, to = {0.0f, 0.0f, 0.0f};
    camera_rig_direction(p->rig, forward);
    glm_vec3_sub((float*)at, (float*)p->reach_from, to);
    const float dist = glm_vec3_norm(to);
    if (dist > reach || dist < 1e-4f || glm_vec3_dot(to, forward) / dist < cosf(cone))
        return FLT_MAX;
    return dist;
}

float player_reach_distance(const Player* p, const vec3 at) {
    return player_reach_within(p, at, REACH, REACH_CONE);
}
