#include <math.h>
#include <stdio.h>
#include <string.h>

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
// and a second has passed with nothing, the second; and as the door swings, the third.
static const char* const LOOK_LINE = "What is this thing...?";
static const char* const OPENS_LINE = "The face... I think it opens.";
static const char* const INSIDE_LINE = "There's something inside.";
#define LOOK_STAY   2.0f
#define BEAT_SECOND 4.25f // the first line's fade in, its stay, its fade out and a second

static const char* const STATE_NAMES[] = {"none", "settling", "raising",  "holding",  "opening",
                                          "open", "taking",   "lowering", "returning"};

static void enter(Inspect* in, InspectState state) {
    in->state = state;
    in->t = 0.0f;
    if (in->trace)
        printf("hold %s frame %zu\n", STATE_NAMES[state], in->engine->total_frames);
}

static float eased(float u) {
    return glm_smooth(glm_clamp_zo(u));
}

static float wrapped(float a) {
    return atan2f(sinf(a), cosf(a));
}

// A frame's place, and the way its +Y points.
static void frame_ray(mat4 m, vec3 at, vec3 dir) {
    glm_vec3_copy(m[3], at);
    glm_vec3_normalize_to(m[1], dir);
}

// The standing eye the pickup is authored from: square to the cage's face on the bench, as far
// off and as high over its foot as the clips have it.
static void pickup_body(Inspect* in) {
    glm_rotate_y(in->cage->bench, GLM_PIf, in->body);
    glm_translate(in->body, (vec3){0.0f, ARMS_PICKUP_DROP, -ARMS_PICKUP_DISTANCE});
}

static void on_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    Inspect* in = user;
    if (in->trace)
        printf("hold event %s frame %zu\n", name, in->engine->total_frames);
    if (!strcmp(name, "look")) {
        hud_think_for(in->hud, LOOK_LINE, LOOK_STAY);
        in->beat = 0.0f;
        in->thoughts = 1;
    } else if (!strcmp(name, "key")) {
        in->carried = true;
    } else if (!strcmp(name, "away")) {
        // Out of sight, so into the bag.
        in->carried = false;
        if (in->take)
            in->take(in->time);
    }
}

void inspect_init(Inspect* in, Engine* engine, Arms* arms, Birdcage* cage, HouseKey* key,
                  Lights* lights, Hud* hud, Player* player, InspectTakeFn take) {
    *in = (Inspect){.engine = engine,
                    .arms = arms,
                    .cage = cage,
                    .key = key,
                    .lights = lights,
                    .hud = hud,
                    .player = player,
                    .take = take,
                    .beat = -1.0f};
    in->rig = create_camera_rig();
    glm_mat4_identity(in->body);
}

void inspect_free(Inspect* in) {
    // The engine is not left looking through a rig that is gone.
    if (in->state != INSPECT_NONE)
        engine_set_camera_rig(in->engine, in->player->rig);
    free_camera_rig(in->rig);
    in->rig = NULL;
}

// The view, the hold on the player and the arms, at the start of a hold.
static void take_hold(Inspect* in) {
    pickup_body(in);
    glm_vec3_copy(in->player->rig->pose.eye, in->from_eye);
    camera_rig_direction(in->player->rig, in->from_dir);
    vec3 look;
    glm_vec3_add(in->from_eye, in->from_dir, look);
    camera_rig_set_pose(in->rig, in->from_eye, look);
    engine_set_camera_rig(in->engine, in->rig);
    player_hold(in->player, true);
    in->spin = 0.0f;
    in->carried = false;
}

void inspect_pick_up(Inspect* in) {
    if (in->state != INSPECT_NONE || !in->arms->animator || !in->cage->node || !in->rig)
        return;
    take_hold(in);
    in->door = 0.0f;
    in->beat = -1.0f;
    in->thoughts = 0;
    arms_play(in->arms, ARMS_CLIP_RAISE, 0.0f);
    enter(in, INSPECT_SETTLING);
}

void inspect_start_held(Inspect* in, bool open) {
    if (in->state != INSPECT_NONE || !in->arms->animator || !in->cage->node || !in->rig)
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
static void turn(Inspect* in, Game* game, float dt) {
    if (input_is_suppressed(&game->input))
        return;
    in->spin += SPIN_PAD * dt * input_action_value(&game->input, "look_x");
    if (in->player->cursor_captured) {
        double dx = 0.0, dy = 0.0;
        input_mouse_delta(&game->input, &dx, &dy);
        in->spin += (float)dx * SPIN_MOUSE;
    }
}

// From what is held to the next clip, the cage turned back square on its way.
static void next(Inspect* in, InspectState state, ArmsClipId clip) {
    in->spin_from = wrapped(in->spin);
    arms_play(in->arms, clip, FADE);
    enter(in, state);
}

// The view goes back to the player's own from where the arms' last looked, and the player's look
// is turned there, so nothing swings round on the way.
static void begin_return(Inspect* in) {
    arms_show(in->arms, false);
    mat4 view = GLM_MAT4_IDENTITY_INIT;
    arms_at(in->arms, ARMS_AT_VIEW, view);
    frame_ray(view, in->from_eye, in->from_dir);
    camera_rig_aim(in->player->rig, atan2f(in->from_dir[0], in->from_dir[2]),
                   asinf(glm_clamp(in->from_dir[1], -1.0f, 1.0f)));
    enter(in, INSPECT_RETURNING);
}

static void let_go(Inspect* in) {
    arms_show(in->arms, false);
    engine_set_camera_rig(in->engine, in->player->rig);
    player_hold(in->player, false);
    lights_flashlight_hold(in->lights, GLM_VEC3_ZERO, (vec3){0.0f, 0.0f, -1.0f}, 0.0f);
    enter(in, INSPECT_NONE);
}

// What this frame's state asks of the input and the clip: the next state, or none.
static void step(Inspect* in, Game* game, float dt) {
    const bool open = input_action_pressed(&game->input, "interact");
    const bool down = input_action_pressed(&game->input, "put_down");
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
            turn(in, game, dt);
            if (in->thoughts == 1 && in->beat >= BEAT_SECOND) {
                hud_think(in->hud, OPENS_LINE);
                in->thoughts = 2;
            }
            if (in->thoughts >= 2 && open)
                next(in, INSPECT_OPENING, ARMS_CLIP_OPEN);
            else if (in->thoughts >= 2 && down)
                next(in, INSPECT_LOWERING, ARMS_CLIP_LOWER);
            break;
        case INSPECT_OPENING:
            in->door =
                ARMS_DOOR_OPEN * eased((s - ARMS_DOOR_FROM) / (ARMS_DOOR_TO - ARMS_DOOR_FROM));
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

// The camera, the cage, its door, the key and the beam where the clip has them this frame, and
// the view glided in from the player's own or out to it.
static void pose(Inspect* in) {
    const bool gliding = in->state == INSPECT_SETTLING || in->state == INSPECT_RETURNING;
    const float u = eased(in->t / (in->state == INSPECT_SETTLING ? SETTLE : RETURN));
    mat4 view = GLM_MAT4_IDENTITY_INIT, cage = GLM_MAT4_IDENTITY_INIT,
         lamp = GLM_MAT4_IDENTITY_INIT;
    arms_at(in->arms, ARMS_AT_VIEW, view);
    vec3 eye = {0.0f, 0.0f, 0.0f}, dir = {0.0f, 0.0f, 1.0f}, look = {0.0f, 0.0f, 0.0f};
    frame_ray(view, eye, dir);
    if (gliding) {
        vec3 to_eye = {0.0f, 0.0f, 0.0f}, to_dir = {0.0f, 0.0f, 1.0f},
             from_eye = {0.0f, 0.0f, 0.0f}, from_dir = {0.0f, 0.0f, 1.0f};
        glm_vec3_copy(eye, to_eye);
        glm_vec3_copy(dir, to_dir);
        glm_vec3_copy(in->from_eye, from_eye);
        glm_vec3_copy(in->from_dir, from_dir);
        if (in->state == INSPECT_RETURNING) {
            glm_vec3_copy(in->player->rig->pose.eye, to_eye);
            camera_rig_direction(in->player->rig, to_dir);
        }
        glm_vec3_lerp(from_eye, to_eye, u, eye);
        glm_vec3_lerp(from_dir, to_dir, u, dir);
        glm_vec3_normalize(dir);
    }
    glm_vec3_add(eye, dir, look);
    camera_rig_set_pose(in->rig, eye, look);

    // Turned back square as the hands open it or set it down.
    if (in->state == INSPECT_OPENING || in->state == INSPECT_LOWERING)
        in->spin = in->spin_from * (1.0f - eased(in->t / UNSPIN));
    if (in->state == INSPECT_RETURNING) {
        glm_mat4_copy(in->cage->bench, cage);
    } else {
        arms_at(in->arms, ARMS_AT_CAGE, cage);
        glm_rotate_y(cage, in->spin, cage);
    }
    birdcage_place(in->cage, cage);
    birdcage_door(in->cage, in->door);
    mat4 key = GLM_MAT4_IDENTITY_INIT;
    if (in->carried)
        arms_at(in->arms, ARMS_AT_KEY, key);
    else
        birdcage_key(in->cage, key);
    house_key_place(in->key, key);

    arms_at(in->arms, ARMS_AT_LAMP, lamp);
    vec3 at = {0.0f, 0.0f, 0.0f}, beam = {0.0f, 0.0f, 1.0f};
    frame_ray(lamp, at, beam);
    const float hold = in->state == INSPECT_SETTLING    ? u
                       : in->state == INSPECT_RETURNING ? 1.0f - u
                                                        : 1.0f;
    lights_flashlight_hold(in->lights, at, beam, hold);
}

void inspect_frame(Inspect* in, Game* game) {
    if (in->state == INSPECT_NONE)
        return;
    const float dt = (float)game->frame_clock.delta;
    in->t += dt;
    in->time = game->time;
    if (in->beat >= 0.0f)
        in->beat += dt;
    // The clip holds its first instant while the view settles, and its last while it goes back.
    const bool playing = in->state != INSPECT_SETTLING && in->state != INSPECT_RETURNING;
    arms_frame(in->arms, in->body, playing ? dt : 0.0f, on_event, in);
    step(in, game, dt);
    if (in->state != INSPECT_NONE)
        pose(in);
}

void inspect_drop(Inspect* in, bool key_had) {
    if (in->state != INSPECT_NONE)
        let_go(in);
    lights_flashlight_hold(in->lights, GLM_VEC3_ZERO, (vec3){0.0f, 0.0f, -1.0f}, 0.0f);
    in->spin = 0.0f;
    in->carried = false;
    in->door = key_had ? ARMS_DOOR_OPEN : 0.0f;
    if (!in->cage->node)
        return;
    birdcage_place(in->cage, in->cage->bench);
    birdcage_door(in->cage, in->door);
    mat4 hang = GLM_MAT4_IDENTITY_INIT;
    birdcage_key(in->cage, hang);
    house_key_place(in->key, hang);
}
