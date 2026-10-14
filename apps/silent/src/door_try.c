#include <math.h>
#include <stdio.h>

#include "door_try.h"

// The door is shaken from RATTLE_LAG seconds after the knob is first turned, and the knob's own
// hits pull the leaf KNOB_SHARE as far as the rattle's.
#define RATTLE_LAG 0.3
#define KNOB_SHARE 0.5f
// A player's try is heard from right beside the door.
#define HAND_VOLUME 1.2f

void door_sounds_load(DoorSounds* s, AudioSystem* audio) {
    *s = (DoorSounds){.audio = audio};
    if (!audio)
        return;
    char path[96];
    for (int i = 0; i < DOOR_KNOB_TAKES; i++) {
        snprintf(path, sizeof(path), "assets/audio/silent/%s", DOOR_KNOB[i].file);
        s->knob[i] = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    }
    snprintf(path, sizeof(path), "assets/audio/silent/%s", DOOR_RATTLE[0].file);
    s->rattle = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    s->slam = audio_sound_from_file(audio, "assets/audio/silent/door_slam.wav", AUDIO_BUS_SFX);
    s->creak = audio_sound_from_file(audio, "assets/audio/silent/door_creak.wav", AUDIO_BUS_SFX);
}

void door_hear(const DoorSounds* s, const Sound* sound, const Door* door, float volume) {
    if (!sound || !s->audio || !door->entity)
        return;
    AudioVoiceDesc d = {.volume = volume};
    glm_vec3_copy(door->entity->position, d.position);
    audio_play_voice(s->audio, sound, &d);
}

// The leaf off its stop around a hit `d` seconds from it: pulled out over the 50 ms before, the
// latch hitting the strike, and let fall back over the 90 ms after.
static float hit_pull(double d) {
    return d < 0.0 ? glm_smoothstep(-0.05f, 0.0f, (float)d)
                   : 1.0f - glm_smoothstep(0.0f, 0.09f, (float)d);
}

// `take` started at `at`, at `u`: each hit's pull as strong as the hit, the largest of them.
static float take_pull(const DoorTake* take, double at, double u) {
    float pull = 0.0f;
    for (int i = 0; i < take->hits; i++)
        pull = fmaxf(pull, take->strength[i] * hit_pull(u - (at + (double)take->at[i])));
    return pull;
}

double door_try_shake_at(int j) {
    return RATTLE_LAG + (double)j * (double)DOOR_RATTLE[0].seconds;
}

void door_try_lay(DoorTry* t, bool shake) {
    double len = 0.0;
    for (int i = 0; i < t->turns; i++) {
        t->turn_at[i] = len;
        len += DOOR_KNOB[t->knob[i]].seconds;
    }
    t->shakes = 0;
    if (shake) {
        t->shakes = (int)ceil((len - RATTLE_LAG) / (double)DOOR_RATTLE[0].seconds);
        t->shakes = t->shakes < 1 ? 1 : t->shakes;
        len = fmax(len, door_try_shake_at(t->shakes));
    }
    t->end = t->at + len;
}

float door_try_pull(const DoorTry* t, double u) {
    float pull = 0.0f;
    for (int i = 0; i < t->turns; i++)
        pull =
            fmaxf(pull, KNOB_SHARE * take_pull(&DOOR_KNOB[t->knob[i]], t->at + t->turn_at[i], u));
    for (int j = 0; j < t->shakes; j++)
        pull = fmaxf(pull, take_pull(&DOOR_RATTLE[0], t->at + door_try_shake_at(j), u));
    return pull;
}

void door_hand_try_begin(DoorHandTry* t, const DoorSounds* s, Door* door, double time, bool shake,
                         float play) {
    const int knob = t->count % DOOR_KNOB_TAKES;
    DoorTry try = {.at = time, .turns = 1, .knob = {knob}};
    door_try_lay(&try, shake);
    *t = (DoorHandTry){door, try, play, time, t->count + 1};
    door_hear(s, s->knob[knob], door, HAND_VOLUME);
}

void door_hand_try_step(DoorHandTry* t, const DoorSounds* s, double now) {
    if (!t->door)
        return;
    const double was = t->last;
    t->last = now;
    for (int j = 0; j < t->try.shakes; j++) {
        const double at = t->try.at + door_try_shake_at(j);
        if (was < at && at <= now)
            door_hear(s, s->rattle, t->door, HAND_VOLUME);
    }
    if (now >= t->try.end)
        door_hand_try_stop(t);
    else
        door_jolt(t->door, t->play * door_try_pull(&t->try, now));
}

void door_hand_try_stop(DoorHandTry* t) {
    if (t->door)
        door_jolt(t->door, 0.0f);
    t->door = NULL;
}
