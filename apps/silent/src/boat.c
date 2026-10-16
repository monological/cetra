#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/animation.h"

#include "boat.h"
#include "boat_shape.h"
#include "lake.h"
#include "layout.h"
#include "library.h"
#include "mats.h"
#include "sounds.h"

#define BOAT_MODEL "assets/models/boat.glb"

// How it goes (spec 13.56): a 3.4 m wooden boat under an old six horsepower, about a little over
// 3 m/s flat out, getting there in a few seconds and coasting a long way. Thrust in m/s/s at full
// throttle, against drag linear and quadratic in the speed; backing it gets REVERSE of that.
#define THRUST    1.6f
#define DRAG_LIN  0.15f
#define DRAG_QUAD 0.11f
#define REVERSE   0.4f
#define TOP_SPEED 3.2f
// The tiller turns the bow by the thrust pushing the stern round and by the water on the hull as
// it moves, so it barely turns at a standstill and best at speed; the turn eases in over YAW_EASE,
// and in a turn the hull slides out a little.
#define TURN_MAX       0.9f // rad/s
#define TURN_BY_THRUST 0.45f
#define TURN_BY_SPEED  0.8f
#define YAW_EASE       0.45f // seconds
#define SLIP_GAIN      0.12f
#define SLIP_DAMP      2.5f // per second
// What the hands do, eased: the throttle and the tiller, per second.
#define THROTTLE_RATE 0.9f
#define TILLER_RATE   2.2f
#define TILLER_SWING  0.45f // radians the motor turns on its swivel at full tiller
// The motor: idle, and how quickly it comes up and runs down; how long it stumbles when it dies.
#define IDLE      0.25f
#define REV_UP    1.2f
#define REV_DOWN  1.6f
#define DIE_TIME  2.4f
#define PROP_REVS 18.0f // turns a second flat out, which a picture blurs into a disc anyway
// The swell on a lake under a breeze: a heave of a couple of centimetres, a degree of roll and less
// of pitch, a bow that lifts and a stern that squats as it goes faster, and a lean into a turn.
#define HEAVE     0.012f
#define PITCH     0.008f
#define ROLL      0.014f
#define BOW_LIFT  0.05f
#define SQUAT     0.025f
#define TURN_LEAN 0.05f
#define VIBRATION 0.0006f // metres the motor shakes on its swivel, running flat out
// The cord: how far the handle comes out on a pull, and how long out and back.
#define CORD_REACH 0.5f
#define CORD_OUT   0.12f
#define CORD_BACK  0.3f
// Bumping: what speed into something is heard, and what it keeps of its turn.
#define KNOCK_SPEED 0.25f
#define BUMP_TURN   0.5f
// Getting in and out: how near the eye must be, how near the mooring and how slow, and how long
// the view eases. Brought round onto its mooring, it eases there over DOCK_EASE.
#define BOARD_REACH 3.2f
#define BOARD_CONE  0.75f
#define MOOR_NEAR   4.0f
#define MOOR_SLOW   0.6f
#define VIEW_EASE   0.6f
#define DOCK_EASE   0.8f
// The motor dies within CUT_NEAR of the marked spot.
#define CUT_NEAR 4.0f
// A ripple dropped every WAKE_PACE metres the propeller moves, while the boat moves faster than
// WAKE_SPEED.
#define WAKE_PACE  0.35f
#define WAKE_SPEED 0.25f
// Against the other loops tools/fetch_sounds.py levels alike. The motor's loop is a recording of
// one idling, so it plays at its own rate idling and faster as the motor opens up.
#define RUN_IDLE_VOLUME 0.35f
#define RUN_FULL_VOLUME 0.75f
#define RUN_IDLE_PITCH  1.0f
#define RUN_FULL_PITCH  1.8f
#define SLAP_VOLUME     0.35f
// The one-shots, under the loop running flat out: chosen by their levels, not yet by ear.
#define PULL_VOLUME  0.7f
#define COUGH_VOLUME 0.55f
#define CATCH_VOLUME 0.55f
#define DIES_VOLUME  0.45f
#define STOP_VOLUME  0.45f
#define KNOCK_VOLUME 0.7f

static float approach(float from, float to, float step) {
    return from < to ? fminf(from + step, to) : fmaxf(from - step, to);
}

// The boat's frame in plan: toward the bow, and to its left.
static void plan_axes(float heading, vec2 fwd, vec2 left) {
    fwd[0] = sinf(heading), fwd[1] = cosf(heading);
    left[0] = cosf(heading), left[1] = -sinf(heading);
}

// A point of the boat's frame, (x left, z toward the bow), in plan at (x, z, heading).
static void plan_point(float x, float z, float heading, float lx, float lz, vec2 out) {
    vec2 fwd = {0.0f, 0.0f}, left = {0.0f, 0.0f};
    plan_axes(heading, fwd, left);
    out[0] = x + lx * left[0] + lz * fwd[0];
    out[1] = z + lx * left[1] + lz * fwd[1];
}

static void voice(const Boat* b, Sound* sound, const vec3 at, float volume, float pitch) {
    if (!b->sounds.audio || !sound)
        return;
    AudioVoiceDesc d = {.volume = volume, .pitch = pitch};
    glm_vec3_copy((float*)at, d.position);
    audio_play_voice(b->sounds.audio, sound, &d);
}

// Where the motor is, from the boat's place in plan: for a sound, which the swell does not move.
static void motor_world(const Boat* b, vec3 out) {
    vec2 at = {0.0f, 0.0f};
    plan_point(b->x, b->z, b->heading, b->motor_at[0], b->motor_at[2], at);
    glm_vec3_copy((vec3){at[0], LAKE_Y + b->motor_at[1], at[1]}, out);
}

// The boat's frame in the world this frame, its swell on it: drawn at (x, z, heading) at `time`.
static void boat_matrix(const Boat* b, float x, float z, float heading, double time, mat4 out) {
    const float t = (float)time, fast = fminf(fabsf(b->speed) / TOP_SPEED, 1.0f);
    const float heave =
        HEAVE * sinf(1.3f * t) + 0.5f * HEAVE * sinf(2.3f * t + 1.0f) - SQUAT * fast;
    const float pitch = PITCH * sinf(1.1f * t + 0.5f) + BOW_LIFT * powf(fast, 1.5f);
    const float roll = ROLL * sinf(0.9f * t + 2.0f) + 0.5f * ROLL * sinf(1.7f * t) -
                       TURN_LEAN * b->yaw_rate * fast;
    glm_translate_make(out, (vec3){x, LAKE_Y + heave, z});
    glm_rotate_y(out, heading, out);
    // The bow up is the model's +z turned toward +y: a turn about +x of minus the angle.
    glm_rotate_x(out, -pitch, out);
    glm_rotate_z(out, roll, out);
}

// ---------------------------------------------------------------------------------------------
// Building

// The boat turned over on the bank, its gunwales on the ground: the model with no motor, bracket
// or tank, rolled about its keel.
static SceneNode* upturned(Engine* engine, Scene* scene) {
    SceneNode* node = library_take(engine, scene, BOAT_MODEL, "boat_upturned", NULL);
    if (!node)
        return NULL;
    static const char* const GONE[] = {"motor", "bracket", "tank"};
    for (size_t i = 0; i < sizeof(GONE) / sizeof(GONE[0]); i++)
        free_node(library_find(node, GONE[i], NULL));
    library_programs(engine, node);
    float x = 0.0f, z = 0.0f, ground = 0.0f, heading = 0.0f;
    lake_bank_boat(&x, &z, &ground, &heading);
    // Its gunwale is the hull's top, its sheer's lowest; turned over it rests there.
    const float gunwale = 0.31f;
    glm_translate_make(node->original_transform, (vec3){x, ground + gunwale, z});
    glm_rotate_y(node->original_transform, heading, node->original_transform);
    glm_rotate_z(node->original_transform, GLM_PIf, node->original_transform);
    node_add_child(scene->root_node, node);
    return node;
}

// The rope from the moored boat's bow eye down to the water and up to the dock's post, in a kit of
// its own beside the lake's, so it can be taken off the graph while the boat is out.
static SceneNode* tie(Kit* lake_kit, const mat4 bow_eye) {
    float x = 0.0f, z = 0.0f, heading = 0.0f;
    vec3 knot = {0.0f, 0.0f, 0.0f};
    lake_mooring(&x, &z, &heading, knot);
    mat4 moored = GLM_MAT4_IDENTITY_INIT;
    glm_translate_make(moored, (vec3){x, LAKE_Y, z});
    glm_rotate_y(moored, heading, moored);
    vec3 bow = {0.0f, 0.0f, 0.0f};
    mat4 at = GLM_MAT4_IDENTITY_INIT;
    glm_mat4_mul(moored, (vec4*)bow_eye, at);
    glm_vec3_copy(at[3], bow);
    vec3 rope[3];
    glm_vec3_copy(bow, rope[0]);
    glm_vec3_copy((vec3){0.5f * (bow[0] + knot[0]), LAKE_Y + 0.1f, 0.5f * (bow[2] + knot[2])},
                  rope[1]);
    glm_vec3_copy(knot, rope[2]);
    Kit kit;
    kit_init_beside(&kit, lake_kit, GLM_VEC3_ZERO);
    kit.draw_distance = lake_kit->draw_distance;
    kit_frame_pipe(&kit, &KIT_WORLD, MAT_POLE, rope, 3, 0.012f, 5);
    SceneNode* node = kit_finish(&kit, "boat_tie");
    kit_keep_beside(lake_kit, &kit);
    return node;
}

// The node named `name` in the boat and its transform as the file has it, said by name if missing.
static SceneNode* part(SceneNode* model, const char* name, mat4 rest) {
    SceneNode* node = library_find(model, name, NULL);
    if (!node) {
        fprintf(stderr, "silent: %s has no '%s'\n", BOAT_MODEL, name);
        return NULL;
    }
    glm_mat4_copy(node->original_transform, rest);
    return node;
}

// Where an empty or a part's origin is in the boat's frame.
static bool where(SceneNode* model, const char* name, vec3 out) {
    mat4 frame = GLM_MAT4_IDENTITY_INIT;
    if (!library_find(model, name, frame))
        return false;
    glm_mat4_mulv3(model->original_transform, frame[3], 1.0f, out);
    return true;
}

bool boat_build(Boat* b, Engine* engine, Scene* scene, Water* water, Kit* lake_kit, float reach) {
    memset(b, 0, sizeof(*b));
    b->scene = scene;
    b->water = water;
    b->node = library_take(engine, scene, BOAT_MODEL, "boat", NULL);
    if (!b->node)
        return false;
    glm_mat4_identity(b->node->original_transform);
    b->motor = part(b->node, "motor", b->motor_rest);
    b->prop = part(b->node, "prop", b->prop_rest);
    b->cord = part(b->node, "cord", b->cord_rest);
    b->rope = part(b->node, "cord_rope", b->rope_rest);
    mat4 bow_eye = GLM_MAT4_IDENTITY_INIT;
    if (!b->motor || !b->prop || !b->cord || !b->rope || !where(b->node, "seat", b->seat) ||
        !library_find(b->node, "bow_eye", bow_eye)) {
        free_node(b->node);
        b->node = NULL;
        return false;
    }
    where(b->node, "prop", b->prop_at);
    where(b->node, "motor", b->motor_at);
    library_programs(engine, b->node);
    // It moves while the game runs, so no capture keeps it, and it is drawn as far as the lake is.
    b->node->capture_hidden = true;
    b->node->draw_distance = reach;
    node_add_child(scene->root_node, b->node);
    b->tie = tie(lake_kit, bow_eye);
    b->upturned = upturned(engine, scene);
    if (b->upturned)
        b->upturned->draw_distance = reach;
    float x = 0.0f, z = 0.0f, heading = 0.0f;
    lake_mooring(&x, &z, &heading, NULL);
    boat_place(b, NULL, x, z, heading, false);
    printf("silent: boat: moored at %.1f, %.1f; %d dry shapes in its hull\n", (double)x, (double)z,
           BOAT_DRY_COUNT);
    return true;
}

void boat_start_audio(Boat* b, AudioSystem* audio) {
    BoatSounds* s = &b->sounds;
    s->audio = audio;
    if (!audio || !b->node)
        return;
    s->run = sounds_loop(audio, "assets/audio/silent/outboard_run.flac", AUDIO_BUS_SFX);
    s->slap = sounds_loop(audio, "assets/audio/silent/hull_slap.flac", AUDIO_BUS_SFX);
    s->pull = audio_sound_from_file(audio, "assets/audio/silent/outboard_pull.wav", AUDIO_BUS_SFX);
    s->cough =
        audio_sound_from_file(audio, "assets/audio/silent/outboard_cough.wav", AUDIO_BUS_SFX);
    s->caught =
        audio_sound_from_file(audio, "assets/audio/silent/outboard_catch.wav", AUDIO_BUS_SFX);
    s->dies = audio_sound_from_file(audio, "assets/audio/silent/outboard_die.wav", AUDIO_BUS_SFX);
    s->stop = audio_sound_from_file(audio, "assets/audio/silent/outboard_stop.wav", AUDIO_BUS_SFX);
    s->knock = audio_sound_from_file(audio, "assets/audio/silent/boat_knock.wav", AUDIO_BUS_SFX);
}

void boat_free(Boat* b) {
    BoatSounds* s = &b->sounds;
    Sound* all[] = {s->run, s->slap, s->pull, s->cough, s->caught, s->dies, s->stop, s->knock};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        free_sound(all[i]);
    memset(s, 0, sizeof(*s));
}

// ---------------------------------------------------------------------------------------------
// Placing, getting in and out

// Where the rider's feet are in the boat at (x, z, heading): under the seated eye, a standing
// body's eye height below it, which a disabled controller only needs for a save.
static void rider_feet(const Boat* b, float x, float z, float heading, vec3 out) {
    vec2 at = {0.0f, 0.0f};
    plan_point(x, z, heading, b->seat[0], b->seat[2], at);
    glm_vec3_copy((vec3){at[0], LAKE_Y + b->seat[1] - PLAYER_EYE_HEIGHT, at[1]}, out);
}

static void set_moored(Boat* b, bool moored) {
    b->moored = moored;
    kit_node_shown(b->scene->root_node, b->tie, moored);
}

void boat_place(Boat* b, Player* p, float x, float z, float heading, bool riding) {
    // Nobody in it, it is at its mooring: the only other place it is left is being brought
    // onto it.
    if (!riding)
        lake_mooring(&x, &z, &heading, NULL);
    b->x = b->prev_x = x;
    b->z = b->prev_z = z;
    b->heading = b->prev_heading = b->drawn_heading = heading;
    b->speed = b->slip = b->yaw_rate = b->throttle = b->tiller = b->rpm = 0.0f;
    b->motor_state = BOAT_MOTOR_OFF;
    b->docking = false;
    pull_cord_close(&b->pull);
    set_moored(b, !riding);
    if (p && riding && !p->riding) {
        player_board(p, 0.0f);
        camera_rig_aim(p->rig, heading, p->rig->pitch);
    } else if (p && !riding && p->riding) {
        // Out where the body now stands, which whatever placed the boat has put it.
        vec3 feet = {0.0f, 0.0f, 0.0f};
        player_feet(p, feet);
        player_disembark(p, feet, p->rig->yaw, 0.0f);
    }
    b->riding = riding;
    b->cut_armed = riding;
    if (riding && p) {
        vec3 feet = {0.0f, 0.0f, 0.0f};
        rider_feet(b, x, z, heading, feet);
        player_carry(p, feet);
    }
}

float boat_reach(const Boat* b, const Player* p) {
    if (!b->node || b->riding || !b->moored)
        return FLT_MAX;
    // The boat's middle at its gunwales, from the eye a hand reaches from.
    vec3 at = {0.0f, 0.0f, 0.0f}, to = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, 0.0f};
    glm_mat4_mulv3(b->node->original_transform, (vec3){0.0f, 0.3f, 0.0f}, 1.0f, at);
    glm_vec3_sub(at, (float*)p->reach_from, to);
    const float dist = glm_vec3_norm(to);
    camera_rig_direction(p->rig, forward);
    if (dist > BOARD_REACH || dist < 1e-4f || glm_vec3_dot(to, forward) / dist < cosf(BOARD_CONE))
        return FLT_MAX;
    return dist;
}

void boat_board(Boat* b, Player* p, Hud* hud) {
    if (b->riding || !b->node)
        return;
    set_moored(b, false);
    b->docking = false;
    b->riding = true;
    b->cut_armed = true;
    player_board(p, VIEW_EASE);
    // Sat facing the bow, looking a little down into the boat.
    camera_rig_aim(p->rig, b->heading, -0.12f);
    if (hud && !b->told)
        hud_think(hud, "It still has a motor on it.");
}

bool boat_riding(const Boat* b) {
    return b->riding;
}

// Alongside the dock and slow enough to step out.
static bool can_get_out(const Boat* b) {
    float mx = 0.0f, mz = 0.0f, mh = 0.0f;
    lake_mooring(&mx, &mz, &mh, NULL);
    return b->riding && b->motor_state != BOAT_MOTOR_PULLING && fabsf(b->speed) < MOOR_SLOW &&
           hypotf(b->x - mx, b->z - mz) < MOOR_NEAR;
}

// Out onto the dock beside the mooring, facing back along it to the shore; the motor stopped, and
// the boat brought round onto its mooring.
static void get_out(Boat* b, Player* p) {
    if (b->motor_state == BOAT_MOTOR_RUNNING) {
        vec3 at = {0.0f, 0.0f, 0.0f};
        motor_world(b, at);
        voice(b, b->sounds.stop, at, STOP_VOLUME, 1.0f);
    }
    b->motor_state = BOAT_MOTOR_OFF;
    b->rpm = 0.0f;
    pull_cord_close(&b->pull);
    b->riding = false;
    b->docking = true;
    float mx = 0.0f, mz = 0.0f, mh = 0.0f;
    lake_mooring(&mx, &mz, &mh, NULL);
    float box[4];
    lake_dock_box(box);
    // On the deck, a stride in from its south edge, level with the boat's middle.
    const vec3 feet = {mx, LAKE_Y + 0.95f, box[1] + 0.3f + 0.45f};
    player_disembark(p, feet, 0.5f * GLM_PIf, VIEW_EASE);
}

const char* boat_prompt(const Boat* b) {
    if (!b->riding)
        return NULL;
    switch (b->motor_state) {
        case BOAT_MOTOR_OFF:
            return can_get_out(b) ? "E   Start the motor      RMB   Get out"
                                  : "E   Start the motor";
        case BOAT_MOTOR_PULLING:
            return "E   Pull the cord      RMB   Let go";
        case BOAT_MOTOR_RUNNING:
            return can_get_out(b) ? "E   Stop the motor      RMB   Get out" : "E   Stop the motor";
        case BOAT_MOTOR_DYING:
            break;
    }
    return NULL;
}

// The view turned to look at the motor's start handle from the seat, easing there: what the pull
// cord wants. Worked in the boat's own frame, which is set before a frame has drawn the seat.
static void look_at_motor(const Boat* b, Player* p) {
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    glm_mat4_mul((vec4*)b->motor_rest, (vec4*)b->cord_rest, m);
    vec3 to = {0.0f, 0.0f, 0.0f};
    glm_vec3_sub(m[3], (float*)b->seat, to);
    vec2 fwd = {0.0f, 0.0f}, left = {0.0f, 0.0f};
    plan_axes(b->heading, fwd, left);
    const float wx = to[0] * left[0] + to[2] * fwd[0], wz = to[0] * left[1] + to[2] * fwd[1];
    camera_rig_blend_from_here(p->rig, VIEW_EASE);
    camera_rig_aim(p->rig, atan2f(wx, wz), asinf(to[1] / fmaxf(glm_vec3_norm(to), 1e-4f)));
}

void boat_open_cord(Boat* b, Player* p, Hud* hud) {
    b->motor_state = BOAT_MOTOR_PULLING;
    b->motor_time = 0.0f;
    pull_cord_open(&b->pull, (unsigned int)(b->clock * 1000.0) * 2654435761u + 17u);
    if (hud)
        hud_set_gauge(hud, pull_cord_draw, &b->pull);
    if (p)
        look_at_motor(b, p);
}

void boat_start_motor(Boat* b) {
    b->motor_state = BOAT_MOTOR_RUNNING;
    b->motor_time = 0.0f;
    b->rpm = IDLE;
    pull_cord_close(&b->pull);
}

// ---------------------------------------------------------------------------------------------
// The fixed step

// The deepest any rim point stands in what the boat may not enter, and which way is out: the
// ground short of the open water, or the dock.
static float deepest(const Boat* b, vec2 out) {
    float worst = 0.0f;
    float box[4];
    lake_dock_box(box);
    for (int i = 0; i <= BOAT_RIM_COUNT; i++) {
        // The rim round, and the motor's leg behind the transom.
        const float lx = i < BOAT_RIM_COUNT ? BOAT_RIM[i][0] : 0.0f;
        const float lz = i < BOAT_RIM_COUNT ? BOAT_RIM[i][1] : BOAT_STERN_Z - 0.3f;
        vec2 p = {0.0f, 0.0f};
        plan_point(b->x, b->z, b->heading, lx, lz, p);
        const float open = lake_open_water(p[0], p[1]);
        if (-open > worst) {
            const float e = 0.2f;
            vec2 g = {lake_open_water(p[0] + e, p[1]) - lake_open_water(p[0] - e, p[1]),
                      lake_open_water(p[0], p[1] + e) - lake_open_water(p[0], p[1] - e)};
            if (glm_vec2_norm(g) > 1e-6f) {
                glm_vec2_normalize(g);
                worst = -open;
                glm_vec2_copy(g, out);
            }
        }
        if (p[0] > box[0] && p[0] < box[2] && p[1] > box[1] && p[1] < box[3]) {
            const float d[4] = {p[0] - box[0], box[2] - p[0], p[1] - box[1], box[3] - p[1]};
            const vec2 n[4] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}};
            int k = 0;
            for (int j = 1; j < 4; j++)
                if (d[j] < d[k])
                    k = j;
            if (d[k] > worst) {
                worst = d[k];
                glm_vec2_copy((float*)n[k], out);
            }
        }
    }
    return worst;
}

// Out of whatever it has run into, losing the speed it had into it.
static void bump(Boat* b) {
    for (int pass = 0; pass < 3; pass++) {
        vec2 n = {0.0f, 0.0f};
        const float depth = deepest(b, n);
        if (depth <= 0.0f)
            return;
        b->x += n[0] * depth;
        b->z += n[1] * depth;
        vec2 fwd = {0.0f, 0.0f}, left = {0.0f, 0.0f};
        plan_axes(b->heading, fwd, left);
        vec2 v = {fwd[0] * b->speed + left[0] * b->slip, fwd[1] * b->speed + left[1] * b->slip};
        const float into = -glm_vec2_dot(v, n);
        if (into > 0.0f) {
            v[0] += n[0] * into;
            v[1] += n[1] * into;
            b->speed = glm_vec2_dot(v, fwd);
            b->slip = glm_vec2_dot(v, left);
            // A knock costs the turn; leaning on the bank does not, or it could never turn off it.
            if (into > KNOCK_SPEED) {
                b->yaw_rate *= BUMP_TURN;
                if (into > b->knock_speed) {
                    b->knocked = true;
                    b->knock_speed = into;
                }
            }
        }
    }
}

// The motor's speed this step: coming up toward what the throttle asks, running down, or
// stumbling to a stop.
static void motor_step(Boat* b, float dt) {
    b->motor_time += dt;
    switch (b->motor_state) {
        case BOAT_MOTOR_RUNNING:
            b->rpm = approach(b->rpm, IDLE + (1.0f - IDLE) * fabsf(b->throttle), REV_UP * dt);
            // The first time on a trip it reaches the marked spot, it dies.
            if (b->cut_armed && hypotf(b->x - LAKE_MARK_X, b->z - LAKE_MARK_Z) < CUT_NEAR) {
                b->cut_armed = false;
                b->motor_state = BOAT_MOTOR_DYING;
                b->motor_time = 0.0f;
                b->dying_from = fmaxf(b->rpm, IDLE);
                vec3 motor = {0.0f, 0.0f, 0.0f};
                motor_world(b, motor);
                voice(b, b->sounds.dies, motor, DIES_VOLUME, 1.0f);
            }
            break;
        case BOAT_MOTOR_DYING: {
            const float t = b->motor_time, left = fmaxf(1.0f - t / DIE_TIME, 0.0f);
            b->rpm = b->dying_from * left * (0.55f + 0.45f * sinf(31.0f * t) * sinf(9.0f * t));
            if (t >= DIE_TIME) {
                b->motor_state = BOAT_MOTOR_OFF;
                b->rpm = 0.0f;
                b->cut_out = true;
            }
            break;
        }
        default:
            b->rpm = approach(b->rpm, 0.0f, REV_DOWN * dt);
            break;
    }
}

void boat_step(Boat* b, Game* game, Player* p, float dt) {
    if (!b->node)
        return;
    b->prev_x = b->x;
    b->prev_z = b->z;
    b->prev_heading = b->heading;
    b->clock += dt;
    motor_step(b, dt);
    if (b->moored)
        return;
    if (b->docking) {
        // Brought round onto its mooring, then tied there.
        float mx = 0.0f, mz = 0.0f, mh = 0.0f;
        lake_mooring(&mx, &mz, &mh, NULL);
        const float k = 1.0f - expf(-dt / (0.3f * DOCK_EASE));
        b->x += (mx - b->x) * k;
        b->z += (mz - b->z) * k;
        b->heading = wrap_pi(b->heading + wrap_pi(mh - b->heading) * k);
        b->speed = b->slip = b->yaw_rate = 0.0f;
        if (hypotf(mx - b->x, mz - b->z) < 0.01f && fabsf(wrap_pi(mh - b->heading)) < 0.005f) {
            b->x = mx, b->z = mz, b->heading = mh;
            b->docking = false;
            set_moored(b, true);
        }
        return;
    }
    // What the hands ask: the throttle only while the motor runs, the tiller any time.
    const bool running = b->motor_state == BOAT_MOTOR_RUNNING;
    const float want_throttle =
        b->riding && running ? input_action_value(&game->input, "move_y") : 0.0f;
    const float want_tiller = b->riding ? -input_action_value(&game->input, "move_x") : 0.0f;
    b->throttle = approach(b->throttle, glm_clamp(want_throttle, -1.0f, 1.0f), THROTTLE_RATE * dt);
    b->tiller = approach(b->tiller, glm_clamp(want_tiller, -1.0f, 1.0f), TILLER_RATE * dt);

    const bool driven = running || b->motor_state == BOAT_MOTOR_DYING;
    const float push = driven ? b->throttle * (b->throttle < 0.0f ? REVERSE : 1.0f) : 0.0f;
    const float thrust =
        THRUST * push * fminf(b->rpm / IDLE, 1.0f) * (driven ? (0.4f + 0.6f * b->rpm) : 0.0f);
    b->speed += (thrust - DRAG_LIN * b->speed - DRAG_QUAD * b->speed * fabsf(b->speed)) * dt;
    const float steer = TURN_MAX * b->tiller *
                        (TURN_BY_THRUST * thrust / THRUST + TURN_BY_SPEED * b->speed / TOP_SPEED);
    b->yaw_rate += (steer - b->yaw_rate) * (1.0f - expf(-dt / YAW_EASE));
    b->slip += (-b->yaw_rate * b->speed * SLIP_GAIN - b->slip * SLIP_DAMP) * dt;
    b->heading = wrap_pi(b->heading + b->yaw_rate * dt);
    vec2 fwd = {0.0f, 0.0f}, left = {0.0f, 0.0f};
    plan_axes(b->heading, fwd, left);
    b->x += (fwd[0] * b->speed + left[0] * b->slip) * dt;
    b->z += (fwd[1] * b->speed + left[1] * b->slip) * dt;
    bump(b);
    if (b->riding) {
        vec3 feet = {0.0f, 0.0f, 0.0f};
        rider_feet(b, b->x, b->z, b->heading, feet);
        player_carry(p, feet);
    }
    if (b->trace && lround(b->clock / dt) % 30 == 0)
        printf("boat %7.2f x %8.3f z %8.3f heading %+.3f speed %+.3f slip %+.3f yaw %+.3f "
               "throttle %+.2f tiller %+.2f rpm %.2f motor %d open %.2f\n",
               b->clock, (double)b->x, (double)b->z, (double)b->heading, (double)b->speed,
               (double)b->slip, (double)b->yaw_rate, (double)b->throttle, (double)b->tiller,
               (double)b->rpm, (int)b->motor_state, (double)lake_open_water(b->x, b->z));
}

// ---------------------------------------------------------------------------------------------
// The frame

// E and RMB in the boat, read once a frame.
static void hands(Boat* b, Game* game, Player* p, Hud* hud) {
    if (!b->riding)
        return;
    vec3 motor = {0.0f, 0.0f, 0.0f};
    motor_world(b, motor);
    const bool e = input_action_pressed(&game->input, "interact");
    const bool back = input_action_pressed(&game->input, "put_down");
    switch (b->motor_state) {
        case BOAT_MOTOR_OFF:
            if (e)
                boat_open_cord(b, p, hud);
            else if (back && can_get_out(b))
                get_out(b, p);
            break;
        case BOAT_MOTOR_PULLING:
            if (e) {
                const bool good = pull_cord_pull(&b->pull);
                b->cord_yank = 0.0f;
                camera_rig_shake(p->rig, good ? 0.012f : 0.006f, 0.25f);
                voice(b, b->sounds.pull, motor, PULL_VOLUME,
                      0.92f + 0.16f * (float)(b->pull.good % 3) / 2.0f);
                if (pull_cord_caught(&b->pull)) {
                    voice(b, b->sounds.caught, motor, CATCH_VOLUME, 1.0f);
                    boat_start_motor(b);
                    camera_rig_blend_from_here(p->rig, VIEW_EASE);
                    camera_rig_aim(p->rig, b->heading, -0.08f);
                    if (hud && !b->told) {
                        hud_hint(hud, "W  S   Throttle      A  D   Steer", 6.0f);
                        b->told = true;
                    }
                } else if (good) {
                    voice(b, b->sounds.cough, motor, COUGH_VOLUME,
                          0.95f + 0.1f * (float)b->pull.good);
                }
            } else if (back) {
                b->motor_state = BOAT_MOTOR_OFF;
                pull_cord_close(&b->pull);
                camera_rig_blend_from_here(p->rig, VIEW_EASE);
                camera_rig_aim(p->rig, b->heading, -0.08f);
            }
            break;
        case BOAT_MOTOR_RUNNING:
            if (e) {
                voice(b, b->sounds.stop, motor, STOP_VOLUME, 1.0f);
                b->motor_state = BOAT_MOTOR_OFF;
                b->motor_time = 0.0f;
            } else if (back && can_get_out(b)) {
                get_out(b, p);
            }
            break;
        case BOAT_MOTOR_DYING:
            break;
    }
}

// The motor turned on its swivel by the tiller and shaken as it runs, its propeller spun, and the
// start handle and its cord out as far as the last pull has them.
static void parts_frame(Boat* b, double time, float dt) {
    const float shake = VIBRATION * b->rpm * sinf(97.0f * (float)time);
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    glm_mat4_copy(b->motor_rest, m);
    glm_translate(m, (vec3){shake, 0.0f, 0.0f});
    glm_rotate_y(m, -TILLER_SWING * b->tiller, m);
    glm_mat4_copy(m, b->motor->original_transform);
    b->prop_angle = fmodf(b->prop_angle + PROP_REVS * 2.0f * GLM_PIf * b->rpm * dt, 2.0f * GLM_PIf);
    glm_mat4_copy(b->prop_rest, m);
    glm_rotate_z(m, b->prop_angle, m);
    glm_mat4_copy(m, b->prop->original_transform);
    b->cord_yank += dt;
    const float y = b->cord_yank;
    const float out = y < CORD_OUT ? y / CORD_OUT : fmaxf(1.0f - (y - CORD_OUT) / CORD_BACK, 0.0f);
    const float reach = CORD_REACH * out * out * (3.0f - 2.0f * out);
    glm_mat4_copy(b->cord_rest, m);
    glm_translate(m, (vec3){0.0f, 0.0f, reach});
    glm_mat4_copy(m, b->cord->original_transform);
    // The rope is a metre long in the file, squashed at rest to almost nothing.
    glm_mat4_copy(b->rope_rest, m);
    const float rest_len = glm_vec3_norm(b->rope_rest[2]);
    glm_scale(m, (vec3){1.0f, 1.0f, fmaxf(reach, 1e-3f) / fmaxf(rest_len, 1e-6f)});
    glm_mat4_copy(m, b->rope->original_transform);
}

// The hull's dry shapes on the water, where the boat is drawn.
static void dry_frame(const Boat* b, float x, float z, float heading) {
    if (!b->water)
        return;
    vec2 fwd = {0.0f, 0.0f}, left = {0.0f, 0.0f};
    plan_axes(heading, fwd, left);
    for (int i = 0; i < BOAT_DRY_COUNT; i++) {
        vec2 c = {0.0f, 0.0f};
        plan_point(x, z, heading, 0.0f, BOAT_DRY[i][0], c);
        b->water->dry[i] = (WaterDry){.centre = {c[0], c[1]},
                                      .along = {fwd[0], fwd[1]},
                                      .half = {BOAT_DRY[i][1], BOAT_DRY[i][2]},
                                      .power = BOAT_DRY[i][3]};
    }
    b->water->dry_count = BOAT_DRY_COUNT;
}

static void sounds_frame(Boat* b, float x, float z) {
    BoatSounds* s = &b->sounds;
    vec3 motor = {0.0f, 0.0f, 0.0f};
    motor_world(b, motor);
    if (s->run) {
        const float r = glm_clamp((b->rpm - IDLE) / (1.0f - IDLE), 0.0f, 1.0f);
        const float on = glm_clamp(b->rpm / IDLE, 0.0f, 1.0f);
        audio_sound_set_position(s->run, motor);
        audio_sound_set_volume(s->run, on * glm_lerp(RUN_IDLE_VOLUME, RUN_FULL_VOLUME, r));
        audio_sound_set_pitch(s->run,
                              glm_lerp(RUN_IDLE_PITCH, RUN_FULL_PITCH, r) * fmaxf(on, 0.5f));
    }
    if (s->slap) {
        audio_sound_set_position(s->slap, (vec3){x, LAKE_Y + 0.1f, z});
        const float fast = fminf(fabsf(b->speed) / TOP_SPEED, 1.0f);
        audio_sound_set_volume(s->slap, SLAP_VOLUME * (0.25f + 0.75f * fast));
    }
    if (b->knocked) {
        voice(b, s->knock, (vec3){x, LAKE_Y + 0.2f, z},
              KNOCK_VOLUME * fminf(0.4f + b->knock_speed, 1.0f), 1.0f);
        b->knocked = false;
        b->knock_speed = 0.0f;
    }
}

void boat_frame(Boat* b, Game* game, Player* p, Hud* hud, double time, float dt) {
    if (!b->node)
        return;
    hands(b, game, p, hud);
    pull_cord_update(&b->pull, dt);
    if (b->cut_out) {
        b->cut_out = false;
        if (hud)
            hud_think(hud, "No... not out here.");
    }
    // Drawn between the last two steps.
    const float alpha = (float)(game->accumulator / game->fixed_timestep);
    const float x = glm_lerp(b->prev_x, b->x, alpha), z = glm_lerp(b->prev_z, b->z, alpha);
    const float heading = wrap_pi(b->prev_heading + wrap_pi(b->heading - b->prev_heading) * alpha);
    boat_matrix(b, x, z, heading, time, b->node->original_transform);
    parts_frame(b, time, dt);
    dry_frame(b, x, z, heading);
    if (b->riding) {
        vec3 eye;
        glm_mat4_mulv3(b->node->original_transform, b->seat, 1.0f, eye);
        player_seat(p, eye, wrap_pi(heading - b->drawn_heading));
    }
    b->drawn_heading = heading;
    // A ripple off the propeller while it moves through the water.
    if (b->water) {
        vec3 wash;
        glm_mat4_mulv3(b->node->original_transform, b->prop_at, 1.0f, wash);
        water_wake(b->water, &b->wake, wash[0], wash[2], WAKE_PACE, fabsf(b->speed) > WAKE_SPEED);
    }
    sounds_frame(b, x, z);
}
