#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/animation.h"
#include "cetra/game/input.h"

#include "inspect.h"

// Seconds the view takes to settle onto the pickup's framing, and to go back after.
#define SETTLE 0.6f
#define RETURN 0.6f
// Seconds one clip crossfades into the next: the clips meet, all but a loop's breath.
#define FADE 0.15f
// Seconds a turned cage takes to come back square to the hands, which open it and set it down
// from there.
#define UNSPIN 0.3f
// How fast it turns on the palm: radians per pixel of the mouse, and per second of the stick.
#define SPIN_MOUSE 0.005f
#define SPIN_PAD   2.5f

// What the player thinks: at the look, a line that stays LOOK_STAY seconds; then, once it has gone
// and BEAT_PAUSE seconds have passed with nothing on the screen, the second; and as the door
// swings, the third.
static const char* const LOOK_LINE = "What is this thing...?";
static const char* const OPENS_LINE = "The face... I think it opens.";
static const char* const INSIDE_LINE = "There's something inside.";
#define LOOK_STAY  2.0f
#define BEAT_PAUSE 1.0f

static const char* const STATE_NAMES[] = {"none", "settling", "raising",  "holding",  "opening",
                                          "open", "taking",   "lowering", "returning"};
_Static_assert(sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) == INSPECT_RETURNING + 1,
               "a name for every state");

static void enter(Inspect* in, InspectState state) {
    in->state = state;
    in->t = 0.0f;
    if (in->trace)
        printf("hold %s frame %zu\n", STATE_NAMES[state], in->game->engine->total_frames);
}

// A frame's place, and the point a step along its +Y.
static void frame_ray(mat4 m, vec3 at, vec3 ahead) {
    glm_vec3_copy(m[3], at);
    glm_vec3_add(m[3], m[1], ahead);
}

static void on_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    Inspect* in = user;
    if (in->trace)
        printf("hold event %s frame %zu\n", name, in->game->engine->total_frames);
    if (!strcmp(name, "look")) {
        hud_think_for(in->hud, LOOK_LINE, LOOK_STAY);
        in->quiet = 0.0f;
        in->thoughts = 1;
    } else if (!strcmp(name, "key")) {
        in->carried = true;
    } else if (!strcmp(name, "away")) {
        // Out of sight, so into the bag.
        in->carried = false;
        if (in->take)
            in->take(in->game->time);
    }
}

void inspect_init(Inspect* in, Game* game, Arms* arms, Birdcage* cage, HouseKey* key,
                  Lights* lights, Hud* hud, Player* player, InspectTakeFn take) {
    *in = (Inspect){.game = game,
                    .arms = arms,
                    .cage = cage,
                    .key = key,
                    .lights = lights,
                    .hud = hud,
                    .player = player,
                    .take = take};
    in->rig = create_camera_rig();
    if (arms->animator)
        animator_set_event_callback(arms->animator, on_event, in);
}

void inspect_free(Inspect* in) {
    // The engine is not left looking through a rig that is gone.
    if (in->state != INSPECT_NONE)
        engine_set_camera_rig(in->game->engine, in->player->rig);
    free_camera_rig(in->rig);
    in->rig = NULL;
}

bool inspect_ready(const Inspect* in) {
    return in->arms->animator && in->cage->node && in->rig;
}

// The view, the hold on the player and the arms, at the start of a hold: the view takes the
// player's and glides from there, and the arms stand at the eye the pickup is authored from --
// square to the cage's face on the bench, as far off and as high over its foot as the clips have
// it.
static void take_hold(Inspect* in) {
    mat4 body = GLM_MAT4_IDENTITY_INIT;
    glm_rotate_y(in->cage->bench, GLM_PIf, body);
    glm_translate(body, (vec3){0.0f, ARMS_PICKUP_DROP, -ARMS_PICKUP_DISTANCE});
    arms_place(in->arms, body);
    camera_rig_set_pose(in->rig, in->player->rig->pose.eye, in->player->rig->pose.look);
    camera_rig_blend_from_here(in->rig, SETTLE);
    engine_set_camera_rig(in->game->engine, in->rig);
    in->player->holding = true;
    in->spin = 0.0f;
    in->carried = false;
}

void inspect_pick_up(Inspect* in) {
    if (in->state != INSPECT_NONE || !inspect_ready(in))
        return;
    take_hold(in);
    in->door = 0.0f;
    in->thoughts = 0;
    arms_play(in->arms, ARMS_CLIP_RAISE, 0.0f);
    enter(in, INSPECT_SETTLING);
}

void inspect_start_held(Inspect* in, bool open) {
    if (in->state != INSPECT_NONE || !inspect_ready(in))
        return;
    take_hold(in);
    in->door = open ? ARMS_DOOR_OPEN : 0.0f;
    in->thoughts = 2;
    arms_play(in->arms, open ? ARMS_CLIP_WAIT : ARMS_CLIP_HOLD, 0.0f);
    arms_show(in->arms, true);
    enter(in, open ? INSPECT_OPEN : INSPECT_HOLDING);
}

bool inspect_active(const Inspect* in) {
    return in->state != INSPECT_NONE;
}

const char* inspect_prompt(const Inspect* in) {
    if (in->state == INSPECT_HOLDING && in->thoughts >= 2)
        return "E   Open    RMB   Put down";
    if (in->state == INSPECT_OPEN)
        return "E   Take the key";
    return NULL;
}

// The cage turned on the palm by the mouse, or the stick and the arrows, while nothing holds the
// game's input.
static void turn(Inspect* in, float dt) {
    GameInputState* input = &in->game->input;
    if (input_is_suppressed(input))
        return;
    in->spin += SPIN_PAD * dt * input_action_value(input, "look_x");
    if (in->player->cursor_captured) {
        double dx = 0.0, dy = 0.0;
        input_mouse_delta(input, &dx, &dy);
        in->spin += (float)dx * SPIN_MOUSE;
    }
}

// From what is held to the next clip, the cage turned back square on its way.
static void next(Inspect* in, InspectState state, ArmsClipId clip) {
    in->spin_from = wrap_pi(in->spin);
    arms_play(in->arms, clip, FADE);
    enter(in, state);
}

// The view goes back to the player's own from where the arms' last looked, and the player's look
// is turned there, so nothing swings round on the way.
static void begin_return(Inspect* in) {
    arms_show(in->arms, false);
    vec3 eye = {0.0f, 0.0f, 0.0f}, look = {0.0f, 0.0f, 0.0f}, dir = {0.0f, 0.0f, 0.0f};
    glm_vec3_copy(in->rig->pose.eye, eye);
    glm_vec3_copy(in->rig->pose.look, look);
    glm_vec3_sub(look, eye, dir);
    glm_vec3_normalize(dir);
    camera_rig_aim(in->player->rig, atan2f(dir[0], dir[2]), asinf(glm_clamp(dir[1], -1.0f, 1.0f)));
    camera_rig_blend_from_here(in->rig, RETURN);
    enter(in, INSPECT_RETURNING);
}

static void let_go(Inspect* in) {
    arms_show(in->arms, false);
    engine_set_camera_rig(in->game->engine, in->player->rig);
    in->player->holding = false;
    lights_flashlight_release(in->lights);
    enter(in, INSPECT_NONE);
}

// What this frame's state asks of the input and the clip: the next state, or none.
static void step(Inspect* in, float dt) {
    const bool open = input_action_pressed(&in->game->input, "interact");
    const bool down = input_action_pressed(&in->game->input, "put_down");
    const float s = arms_seconds(in->arms);
    switch (in->state) {
        case INSPECT_SETTLING:
            if (in->t >= SETTLE) {
                arms_show(in->arms, true);
                enter(in, INSPECT_RAISING);
            }
            break;
        case INSPECT_RAISING:
            if (arms_done(in->arms))
                next(in, INSPECT_HOLDING, ARMS_CLIP_HOLD);
            break;
        case INSPECT_HOLDING:
            turn(in, dt);
            if (in->thoughts == 1 && in->quiet >= BEAT_PAUSE) {
                hud_think(in->hud, OPENS_LINE);
                in->thoughts = 2;
            }
            if (in->thoughts >= 2 && open)
                next(in, INSPECT_OPENING, ARMS_CLIP_OPEN);
            else if (in->thoughts >= 2 && down)
                next(in, INSPECT_LOWERING, ARMS_CLIP_LOWER);
            break;
        case INSPECT_OPENING:
            in->door = ARMS_DOOR_OPEN * glm_smoothstep(ARMS_DOOR_FROM, ARMS_DOOR_TO, s);
            if (in->thoughts == 2 && s >= ARMS_DOOR_TO) {
                hud_think(in->hud, INSIDE_LINE);
                in->thoughts = 3;
            }
            if (arms_done(in->arms))
                next(in, INSPECT_OPEN, ARMS_CLIP_WAIT);
            break;
        case INSPECT_OPEN:
            if (open)
                next(in, INSPECT_TAKING, ARMS_CLIP_TAKE);
            break;
        case INSPECT_TAKING:
            if (arms_done(in->arms))
                next(in, INSPECT_LOWERING, ARMS_CLIP_LOWER);
            break;
        case INSPECT_LOWERING:
            if (arms_done(in->arms))
                begin_return(in);
            break;
        case INSPECT_RETURNING:
            if (in->t >= RETURN)
                let_go(in);
            break;
        case INSPECT_NONE:
            break;
    }
}

// The cage at `at`, its door at the hold's angle, and the key in the pinch or on its hook.
static void place(Inspect* in, mat4 at) {
    birdcage_place(in->cage, at);
    birdcage_door(in->cage, in->door);
    mat4 key = GLM_MAT4_IDENTITY_INIT;
    if (in->carried)
        arms_at(in->arms, ARMS_AT_KEY, key);
    else
        birdcage_key(in->cage, key);
    house_key_place(in->key, key);
}

// The camera, the cage and the beam where the clip has them this frame. The view glides from the
// player's own on the way in and to it on the way out, by the rig's own blend.
static void pose(Inspect* in, float dt) {
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    vec3 eye = {0.0f, 0.0f, 0.0f}, look = {0.0f, 0.0f, 0.0f};
    if (in->state == INSPECT_RETURNING) {
        glm_vec3_copy(in->player->rig->pose.eye, eye);
        glm_vec3_copy(in->player->rig->pose.look, look);
    } else {
        arms_at(in->arms, ARMS_AT_VIEW, m);
        frame_ray(m, eye, look);
    }
    camera_rig_set_pose(in->rig, eye, look);
    camera_rig_update(in->rig, dt, 0.0f, 0.0f);

    // Turned back square as the hands open it or set it down.
    if (in->state == INSPECT_OPENING || in->state == INSPECT_LOWERING)
        in->spin = in->spin_from * (1.0f - glm_smoothstep(0.0f, UNSPIN, in->t));
    if (in->state == INSPECT_RETURNING) {
        place(in, in->cage->bench);
    } else {
        arms_at(in->arms, ARMS_AT_CAGE, m);
        glm_rotate_y(m, in->spin, m);
        place(in, m);
    }

    // On the cage while it is held, and onto it and off it as the view settles and goes back.
    const float hold = in->state == INSPECT_SETTLING    ? glm_smoothstep(0.0f, SETTLE, in->t)
                       : in->state == INSPECT_RETURNING ? 1.0f - glm_smoothstep(0.0f, RETURN, in->t)
                                                        : 1.0f;
    arms_at(in->arms, ARMS_AT_LAMP, m);
    vec3 beam = {0.0f, 0.0f, 0.0f};
    glm_vec3_normalize_to(m[1], beam);
    lights_flashlight_hold(in->lights, m[3], beam, hold);
}

void inspect_frame(Inspect* in) {
    if (in->state == INSPECT_NONE)
        return;
    const float dt = (float)in->game->frame_clock.delta;
    in->t += dt;
    // The clip holds its first instant while the view settles, and its last while it goes back.
    const bool playing = in->state != INSPECT_SETTLING && in->state != INSPECT_RETURNING;
    arms_frame(in->arms, playing ? dt : 0.0f);
    // The beat's pause is a stretch with nothing thought on the screen, whatever was thought.
    if (in->thoughts == 1)
        in->quiet = hud_thinking(in->hud) ? 0.0f : in->quiet + dt;
    step(in, dt);
    if (in->state != INSPECT_NONE)
        pose(in, dt);
}

void inspect_drop(Inspect* in, bool key_had) {
    if (in->state != INSPECT_NONE)
        let_go(in);
    in->carried = false;
    in->door = key_had ? ARMS_DOOR_OPEN : 0.0f;
    if (in->cage->node)
        place(in, in->cage->bench);
}
