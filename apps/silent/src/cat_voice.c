#include "cat_voice.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cat_clips.h"
#include "cat_places.h"
#include "interior.h"
#include "kit.h"
#include "layout.h"
#include "mansion.h"
#include "sounds.h"

// How loud each is, against silent's own: the fridge 0.15, the clock 0.5, the tubes 0.25.
#define MEOW_VOLUME      0.45f
#define TRILL_VOLUME     0.30f
#define HISS_VOLUME      0.40f
#define PAW_VOLUME       0.05f // on bare boards
#define PAW_RAIL_VOLUME  0.04f // on the hand rail
#define LAND_HARD_VOLUME 0.25f
#define LAND_SOFT_VOLUME 0.12f
#define PURR_VOLUME      0.30f

// The purr is heard within PURR_REACH, fully by PURR_FULL; it swells and fades over PURR_EASE.
#define PURR_REACH 2.0f
#define PURR_FULL  0.7f
#define PURR_EASE  0.6f

#define SAY_EVERY 1.5f // --cat-say: seconds between one sound and the next

static Sound* load(AudioSystem* audio, const char* name) {
    char path[160];
    snprintf(path, sizeof(path), "assets/audio/silent/%s", name);
    Sound* s = audio_sound_from_file_async(audio, path, AUDIO_BUS_SFX);
    if (!s)
        fprintf(stderr, "silent: cannot load %s\n", path);
    return s;
}

// Where on the cat a sound comes from, in its entity's frame: the entity stands at the middle
// of the body, so the feet are its half height down and the head is the rig's, moved up to it.
static const vec3 FEET = {0.0f, -CAT_HALF_HEIGHT, 0.0f};
static const vec3 HEAD = {0.0f, CAT_HEAD_Y - CAT_HALF_HEIGHT, CAT_HEAD_Z};
static const vec3 CHEST = {0.0f, 0.2f - CAT_HALF_HEIGHT, 0.08f};

static void play(CatVoice* v, const Sound* s, const vec3 at, float volume) {
    if (!v->audio || !s || volume <= 0.0f)
        return;
    AudioVoiceDesc d = {.follow = v->cat->entity, .volume = volume};
    glm_vec3_copy((float*)at, d.position);
    audio_play_voice(v->audio, s, &d);
}

// Whether its feet are on a rug, a runner or the window seat's cushion, where a paw makes no
// sound. Those are in the house's plan, which the mansion stands at.
static bool on_cloth(const Cat* cat) {
    vec3 feet = {0.0f, 0.0f, 0.0f};
    cat_feet(cat, feet);
    const vec3 plan = {feet[0] - MANSION_X, feet[1] - MANSION_Y, feet[2] - MANSION_Z};
    return interior_on_cloth(plan) || mansion_on_cloth(plan);
}

// A paw on the rail is quieter than on the boards, and on cloth it makes no sound at all.
static float paw_volume(const Cat* cat) {
    if (on_cloth(cat))
        return 0.0f;
    const NavLink* l = cat->leg >= 0 ? &cat->places->links[cat->leg] : NULL;
    return l && l->kind == CAT_LINK_RAIL ? PAW_RAIL_VOLUME : PAW_VOLUME;
}

// A clip's event, played as the sound it means.
static void heard(Animator* animator, const char* name, void* user) {
    (void)animator;
    CatVoice* v = user;
    if (!strcmp(name, "meow")) {
        // Never the same one twice running.
        v->meows++;
        play(v, v->meow[(v->meows + v->meows / 3) % 3], HEAD, MEOW_VOLUME);
    } else if (!strcmp(name, "trill")) {
        play(v, v->trill, HEAD, TRILL_VOLUME);
    } else if (!strcmp(name, "hiss")) {
        play(v, v->hiss, HEAD, HISS_VOLUME);
    } else if (!strncmp(name, "paw_", 4)) {
        play(v, v->paw[v->paws++ % 4], FEET, paw_volume(v->cat));
    } else if (!strcmp(name, "land") || !strcmp(name, "land_hind")) {
        // Down from a height the fore paws land hard and the hind after them softly; up onto
        // something, and onto cloth, it lands softly.
        const bool soft =
            v->cat->clip != CAT_CLIP_JUMP_DOWN || !strcmp(name, "land_hind") || on_cloth(v->cat);
        play(v, soft ? v->land_soft : v->land_hard, FEET,
             soft ? LAND_SOFT_VOLUME : LAND_HARD_VOLUME);
    }
}

void cat_voice_start(CatVoice* v, Cat* cat, AudioSystem* audio, bool say) {
    memset(v, 0, sizeof(*v));
    v->cat = cat;
    v->say = say;
    if (!audio || !cat->entity)
        return;
    v->audio = audio;
    static const char* MEOWS[] = {"cat_meow_1.wav", "cat_meow_2.wav", "cat_meow_3.wav"};
    static const char* PAWS[] = {"cat_paw_1.wav", "cat_paw_2.wav", "cat_paw_3.wav",
                                 "cat_paw_4.wav"};
    for (int i = 0; i < KIT_COUNT(MEOWS); i++)
        v->meow[i] = load(audio, MEOWS[i]);
    for (int i = 0; i < KIT_COUNT(PAWS); i++)
        v->paw[i] = load(audio, PAWS[i]);
    v->trill = load(audio, "cat_trill.wav");
    v->hiss = load(audio, "cat_hiss.wav");
    v->land_hard = load(audio, "cat_land_hard.wav");
    v->land_soft = load(audio, "cat_land_soft.wav");
    Sound* purr = sounds_loop(audio, "assets/audio/silent/cat_purr.flac");
    if (purr) {
        v->purr = entity_add_audio_source_at(cat->entity, purr, CHEST);
        if (!v->purr)
            free_sound(purr);
    }
    animator_set_event_callback(cat->animator, heard, v);
}

// --cat-say: every sound in turn, for an offline dump to be listened to.
static void say(CatVoice* v, float dt) {
    v->say_in -= dt;
    if (v->say_in > 0.0f)
        return;
    v->say_in = SAY_EVERY;
    static const char* EVENTS[] = {"meow",   "meow",   "meow",   "trill", "hiss",     "paw_lh",
                                   "paw_lf", "paw_rh", "paw_rf", "land",  "land_hind"};
    heard(v->cat->animator, EVENTS[v->say_next++ % KIT_COUNT(EVENTS)], v);
}

void cat_voice_update(CatVoice* v, const vec3 listener, bool at_ease, float dt) {
    const Cat* cat = v->cat;
    if (!v->audio || !cat->entity || !cat->attached)
        return;

    // A purr only settled -- sitting, lying or asleep -- and only for someone close and calm.
    mat4 body = GLM_MAT4_IDENTITY_INIT;
    vec3 chest = {0.0f, 0.0f, 0.0f};
    entity_get_transform_matrix(cat->entity, body);
    glm_mat4_mulv3(body, (float*)CHEST, 1.0f, chest);
    const float near =
        glm_smoothstep(PURR_REACH, PURR_FULL, glm_vec3_distance(chest, (float*)listener));
    const bool settled = cat_settled(cat) && cat->posture != CAT_STAND;
    const float want = (settled && (at_ease || v->say)) ? near : 0.0f;
    v->purr_level += (want - v->purr_level) * (1.0f - expf(-dt / PURR_EASE));
    if (v->purr)
        audio_sound_set_volume(v->purr, PURR_VOLUME * v->purr_level);
    if (v->say)
        say(v, dt);
}
