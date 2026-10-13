#include <math.h>
#include <stdio.h>

#include "door_try.h"

// The rattle comes this long after the knob is turned, and the knob's own hits pull the leaf this
// share as far as the rattle's.
#define TRY_LAG  0.3
#define TRY_KNOB 0.5f

// The leaf off its stop around a hit `d` seconds from it: pulled out over the 50 ms before, the
// latch hitting the strike, and let fall back over the 90 ms after.
static float hit_pull(double d) {
    return d < 0.0 ? glm_smoothstep(-0.05f, 0.0f, (float)d)
                   : 1.0f - glm_smoothstep(0.0f, 0.09f, (float)d);
}

float door_take_pull(const DoorTake* take, double at, double u) {
    float pull = 0.0f;
    for (int i = 0; i < take->hits; i++)
        pull = fmaxf(pull, take->strength[i] * hit_pull(u - (at + (double)take->at[i])));
    return pull;
}

void door_try_takes_load(DoorTryTakes* takes, AudioSystem* audio) {
    *takes = (DoorTryTakes){.audio = audio};
    if (!audio)
        return;
    char path[96];
    for (int i = 0; i < DOOR_KNOB_TAKES; i++) {
        snprintf(path, sizeof(path), "assets/audio/silent/%s", DOOR_KNOB[i].file);
        takes->knob[i] = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    }
    snprintf(path, sizeof(path), "assets/audio/silent/%s", DOOR_RATTLE[0].file);
    takes->rattle = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
}

// A copy of `s` heard at `door`'s leaf.
static void hear(const DoorTryTakes* takes, const Sound* s, const Door* door, float volume) {
    if (!s || !takes->audio || !door->entity)
        return;
    AudioVoiceDesc d = {.volume = volume, .pitch = 1.0f};
    glm_vec3_copy(door->entity->position, d.position);
    audio_play_voice(takes->audio, s, &d);
}

// When the try is over: its knob's take played out, and its rattle's after it.
static double try_end(const DoorTry* t) {
    double len = DOOR_KNOB[t->knob].seconds;
    if (t->shake)
        len = fmax(len, TRY_LAG + DOOR_RATTLE[0].seconds);
    return t->at + len;
}

void door_try_begin(DoorTry* t, const DoorTryTakes* takes, Door* door, double time, int knob,
                    bool shake, float play, float volume) {
    if (!door || !door->entity)
        return;
    knob = ((knob % DOOR_KNOB_TAKES) + DOOR_KNOB_TAKES) % DOOR_KNOB_TAKES;
    *t = (DoorTry){door, time, knob, shake, play, volume};
    hear(takes, takes->knob[knob], door, volume);
}

void door_try_step(DoorTry* t, const DoorTryTakes* takes, double was, double now) {
    if (!t->door)
        return;
    const double rattle = t->at + TRY_LAG;
    if (t->shake && was < rattle && rattle <= now)
        hear(takes, takes->rattle, t->door, t->volume);
    if (now >= try_end(t)) {
        door_jolt(t->door, 0.0f);
        t->door = NULL;
        return;
    }
    float pull = TRY_KNOB * door_take_pull(&DOOR_KNOB[t->knob], t->at, now);
    if (t->shake)
        pull = fmaxf(pull, door_take_pull(&DOOR_RATTLE[0], rattle, now));
    const float width = t->door->shape.to - t->door->shape.from;
    door_jolt(t->door, t->play / width * pull);
}
