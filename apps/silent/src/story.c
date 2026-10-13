#include <math.h>
#include <stdio.h>
#include <string.h>

#include "basement.h"
#include "mats.h"
#include "rooms.h"
#include "save_game.h"
#include "story.h"

const char* const STORY_STAGE_NAMES[STORY_STAGE_COUNT] = {"locked", "haunted", "over", "open"};

StoryStage story_stage_by_name(const char* name) {
    for (int i = 0; name && i < STORY_STAGE_COUNT; i++)
        if (!strcmp(name, STORY_STAGE_NAMES[i]))
            return (StoryStage)i;
    return STORY_NONE;
}

// The key: KEY_LEN from its bow to the end of its bit, its shank KEY_SHANK about its middle, its
// bow a ring of KEY_WIRE KEY_BOW across, and its bit KEY_BIT thick either side of its middle.
#define KEY_LEN   0.1f
#define KEY_SHANK 0.004f
#define KEY_BOW   0.03f
#define KEY_WIRE  0.0035f
#define KEY_BIT   0.0025f
// Where it lies on the basement's workbench, in the bench's frame (BASEMENT_BENCH): in the clear
// stretch between the vise and the jar, turned off square as if it had been put down in a hurry.
#define KEY_BENCH_A    0.85f
#define KEY_BENCH_D    0.3f
#define KEY_BENCH_TURN 1.2f

// How the key lies in a frame: on its side on y, or standing on its edge about y.
typedef struct KeyLay {
    const KitFrame* f;
    float y;
    bool flat;
} KeyLay;

// A point of the key in its frame, `u` along it from its bow, `v` across it toward its bit, `w`
// through its thickness.
static void key_point(const KeyLay* k, float u, float v, float w, vec3 out) {
    const float d = u - 0.5f * KEY_LEN;
    if (k->flat)
        glm_vec3_copy((vec3){v, k->y + w, d}, out);
    else
        glm_vec3_copy((vec3){w, k->y + v, d}, out);
}

// A piece of the bit, u0..u1 along the key and v0..v1 across it, its thickness KEY_BIT either
// side.
static void key_plate(Kit* kit, const KeyLay* k, float u0, float u1, float v0, float v1) {
    vec3 lo = GLM_VEC3_ZERO_INIT, hi = GLM_VEC3_ZERO_INIT;
    key_point(k, u0, v0, -KEY_BIT, lo);
    key_point(k, u1, v1, KEY_BIT, hi);
    kit_frame_box(kit, k->f, MAT_BRASS, fminf(lo[0], hi[0]), fmaxf(lo[0], hi[0]),
                  fminf(lo[1], hi[1]), fmaxf(lo[1], hi[1]), fminf(lo[2], hi[2]),
                  fmaxf(lo[2], hi[2]), false);
}

void story_key_model(Kit* kit, const KitFrame* f, float y, bool flat) {
    const KeyLay k = {f, y, flat};
    // The bow, a ring of the wire, the shank coming out of its far side.
    const float rc = 0.5f * KEY_BOW - KEY_WIRE;
    vec3 ring[13];
    for (int i = 0; i < 13; i++) {
        const float t = (float)i * GLM_PIf / 6.0f;
        key_point(&k, 0.5f * KEY_BOW + rc * cosf(t), rc * sinf(t), 0.0f, ring[i]);
    }
    kit_frame_pipe(kit, f, MAT_BRASS, ring, KIT_COUNT(ring), KEY_WIRE, 6);
    // The shank, round, to the end of the bit, with a collar where it leaves the bow.
    const float from = KEY_BOW - KEY_WIRE, len = KEY_LEN - from, collar = 1.5f * KEY_SHANK;
    const vec2 shank[] = {{0.0f, 0.0f},     {KEY_SHANK, 0.0f}, {KEY_SHANK, 0.004f},
                          {collar, 0.005f}, {collar, 0.009f},  {KEY_SHANK, 0.01f},
                          {KEY_SHANK, len}, {0.0f, len}};
    vec3 base = GLM_VEC3_ZERO_INIT;
    key_point(&k, from, 0.0f, 0.0f, base);
    kit_frame_lathe_on(kit, f, MAT_BRASS, base, (vec3){0.0f, 0.0f, 1.0f}, shank, KIT_COUNT(shank),
                       10);
    // The bit across the shank's end, a ward cut into its middle.
    key_plate(kit, &k, KEY_LEN - 0.018f, KEY_LEN - 0.012f, 0.0f, 0.016f);
    key_plate(kit, &k, KEY_LEN - 0.012f, KEY_LEN - 0.008f, 0.0f, 0.009f);
    key_plate(kit, &k, KEY_LEN - 0.008f, KEY_LEN, 0.0f, 0.016f);
}

// A locked front door is yanked hard enough to rattle in its frame, the leaf pulled FRONT_PLAY
// metres off its stop at its latch edge; a stuck cellar door only gives a little. Both are heard
// from right beside them.
#define FRONT_PLAY  0.01f
#define CELLAR_PLAY 0.004f
#define TRY_VOLUME  1.2f
// The flung cellar door against its stop, against the one-shots tools/fetch_sounds.py levels alike.
#define SLAM_VOLUME 2.0f

static void think(Story* s, const char* text) {
    if (s->house.hud)
        hud_think(s->house.hud, text);
}

static void say(const Story* s, const char* what, double time) {
    if (s->trace)
        printf("story %s at %.3f\n", what, time);
}

// The player's try at `door`, which will not open: its knob turned, and when `shake`, its leaf
// pulled against the latch.
static void try_door(Story* s, Door* door, double time, bool shake, float play) {
    door_try_begin(&s->try, &s->takes, door, time, s->tries++, shake, play, TRY_VOLUME);
}

static bool door_shut(const Door* door) {
    return door->travel <= 0.0f && door->want <= 0.0f;
}

// The front door, locked from the inside. With the backpack -- whose flashlight is what lets the
// player down the cellar stair to the key -- the first try begins the haunting. The key opens it,
// once the player has the map off the fridge too: false then, and the door is the player's.
static bool front_door(Story* s, Door* door, double time) {
    const Backpack* bp = s->house.backpack;
    if (bp && backpack_holds(bp, ITEM_KEY)) {
        if (!backpack_holds(bp, ITEM_TOWN_MAP)) {
            think(s, "I think I saw a map in the kitchen. I might need it.");
            say(s, "front door, the key but no map", time);
            return true;
        }
        think(s, "It fits.");
        s->stage = STORY_OPEN;
        save_game_due();
        say(s, "front door unlocked", time);
        return false;
    }
    try_door(s, door, time, true, FRONT_PLAY);
    const bool bag = bp && bp->taken;
    if (!bag) {
        think(s, "It's locked. From the inside? I need a key... and my bag.");
        say(s, "front door locked, no bag", time);
        return true;
    }
    if (s->stage == STORY_LOCKED) {
        think(s, "It's locked from the inside. I need to find a key.");
        if (s->house.haunt)
            haunt_begin(s->house.haunt, time, 0.0);
        s->stage = STORY_HAUNTED;
        save_game_due();
        say(s, "front door locked, the haunting begun", time);
        return true;
    }
    think(s, "I need to find the key.");
    say(s, "front door locked", time);
    return true;
}

// The cellar door: stuck until the haunting has begun, so nobody reaches the key down there
// before it. Opened while the house is haunted, it ends the haunting first -- the lights, the
// power and the colour back -- and then flies open and slams against its stop, on an ordinary
// stair.
static bool cellar_door(Story* s, Door* door, double time) {
    if (s->house.haunt && haunt_active(s->house.haunt)) {
        haunt_end(s->house.haunt);
        door_slam(door, 1.0f);
        s->slamming = true;
        if (s->stage < STORY_OVER)
            s->stage = STORY_OVER;
        save_game_due();
        say(s, "cellar door flung open, the haunting over", time);
        return true;
    }
    if (s->stage != STORY_LOCKED || !door_shut(door))
        return false;
    try_door(s, door, time, false, CELLAR_PLAY);
    think(s, "It won't open. It's stuck.");
    say(s, "cellar door stuck", time);
    return true;
}

void story_start(Story* s, const StoryHouse* house) {
    *s = (Story){.house = *house, .stage = STORY_LOCKED, .last = -1.0};
    door_try_takes_load(&s->takes, house->audio);
    if (house->audio)
        s->slam =
            audio_sound_from_file(house->audio, "assets/audio/silent/door_slam.wav", AUDIO_BUS_SFX);
    if (!house->engine || !house->scene)
        return;
    // The key on the workbench, in a kit of its own so it can be taken.
    KitFrame f = {{0.0f, 0.0f, 0.0f}, BASEMENT_BENCH.yaw + KEY_BENCH_TURN};
    kit_frame_point(&BASEMENT_BENCH, KEY_BENCH_A, 0.0f, KEY_BENCH_D, f.origin);
    Kit kit;
    mats_kit(&kit, house->engine, house->scene);
    story_key_model(&kit, &f, BASEMENT_BENCH_TOP + KEY_SHANK, true);
    s->key = kit_finish(&kit, "front_door_key");
    // No capture keeps it: it is taken while the game runs.
    s->key->capture_hidden = true;
    glm_vec3_copy(f.origin, s->key_at);
    s->key_at[1] += BASEMENT_BENCH_TOP;
}

void story_free(Story* s) {
    if (s->key)
        free_node(s->key);
    s->key = NULL;
}

void story_key_on_bench(Story* s, bool on) {
    if (s->key && s->house.scene)
        kit_node_shown(s->house.scene->root_node, s->key, on);
}

bool story_door(Story* s, Door* door, double time) {
    if (!door)
        return false;
    if (door == s->house.front && s->stage < STORY_OPEN)
        return front_door(s, door, time);
    if (door == s->house.cellar)
        return cellar_door(s, door, time);
    return false;
}

void story_step(Story* s, double time) {
    const double was = s->last < 0.0 ? time : s->last;
    s->last = time;
    door_try_step(&s->try, &s->takes, was, time);
    // The flung cellar door heard as it hits its stop.
    Door* cellar = s->house.cellar;
    if (s->slamming && cellar && !cellar->slam) {
        s->slamming = false;
        if (s->slam && s->house.audio && cellar->entity) {
            AudioVoiceDesc d = {.volume = SLAM_VOLUME, .pitch = 1.0f};
            glm_vec3_copy(cellar->entity->position, d.position);
            audio_play_voice(s->house.audio, s->slam, &d);
        }
        say(s, "cellar door slammed", time);
    }
}

// Whether `at` is in the player's house, its basement included.
static bool in_house(const vec3 at) {
    const RoomId room = room_at(at);
    return room == ROOM_HOME || room == ROOM_LIVING || room == ROOM_KITCHEN ||
           room == ROOM_BASEMENT;
}

void story_restore(Story* s, StoryStage stage, const vec3 feet) {
    s->stage = stage < STORY_LOCKED || stage >= STORY_STAGE_COUNT ? STORY_LOCKED : stage;
    if (s->stage < STORY_OPEN && !in_house(feet))
        s->stage = STORY_OPEN;
    if (s->try.door)
        door_jolt(s->try.door, 0.0f);
    s->try = (DoorTry){0};
    s->slamming = false;
    s->last = -1.0;
    if (s->stage < STORY_OPEN && s->house.front)
        door_set(s->house.front, 0.0f, 0.0f);
    story_key_on_bench(s, !(s->house.backpack && backpack_holds(s->house.backpack, ITEM_KEY)));
    s->rehaunt = s->stage == STORY_HAUNTED;
}

void story_play(Story* s, double time) {
    if (!s->rehaunt)
        return;
    s->rehaunt = false;
    if (s->house.haunt)
        haunt_begin(s->house.haunt, time, 0.0);
    say(s, "haunted again", time);
}
