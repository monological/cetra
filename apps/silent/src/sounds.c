#include <math.h>
#include <stdio.h>

#include "home.h"
#include "house.h"
#include "kitchen.h"
#include "layout.h"
#include "sounds.h"

// Every loop is levelled alike by tools/fetch_sounds.py, so these are the
// whole statement of how loud each one is.
#define FRIDGE_VOLUME 0.15f
#define WIND_VOLUME   0.6f

#define WALL_BLEND     1.0f // metres across which INSIDE goes from 0 to 1, centred on the walls
#define INSIDE_SECONDS 0.4f // how far behind the listener INSIDE lags, stepping through a door
#define STREET_GAIN    0.3f // what is left of the house's own sounds out on the street

// Above and below each other the floor between is most of the sound, except through the great
// hall, which is open from its floor to the gallery.
#define OTHER_STOREY 0.5f
#define STOREY_APART 2.0f // metres between a listener and a source that put them a floor apart

// Down in the basement (spec 13.31): the eye heights between which the listener goes from above
// the ground to under it, and what the earth and the floor overhead leave of the wind and of the
// house's own sounds.
#define BELOW_FROM     0.6f
#define BELOW_TO       (-0.6f)
#define BELOW_WIND     0.25f
#define BELOW_OVERHEAD 0.4f

Sound* sounds_loop(AudioSystem* audio, const char* path) {
    if (!audio)
        return NULL;
    Sound* s = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    if (!s) {
        fprintf(stderr, "silent: cannot load %s\n", path);
        return NULL;
    }
    audio_sound_set_looping(s, true);
    audio_sound_set_volume(s, 0.0f);
    audio_sound_play(s);
    return s;
}

/*
 * 1 well inside either house's footprint, 0 well outside both, blended over
 * WALL_BLEND across its walls. The front doorway is an open hole, so a
 * listener standing in it is half outside, which is right.
 */
static float inside_target(const vec3 eye) {
    const vec3 plan = {eye[0] - MANSION_X, eye[1] - MANSION_Y, eye[2] - MANSION_Z};
    const float outside = fminf(home_outside_distance(eye), house_outside_distance(plan));
    return glm_smoothstep(0.5f * WALL_BLEND, -0.5f * WALL_BLEND, outside);
}

void sounds_start(Sounds* sounds, AudioSystem* audio, const vec3 eye) {
    sounds->fridge = sounds_loop(audio, "assets/audio/silent/fridge_hum.flac");
    sounds->wind_outside = sounds_loop(audio, "assets/audio/silent/wind_outside.flac");
    sounds->wind_inside = sounds_loop(audio, "assets/audio/silent/wind_inside.flac");
    if (sounds->fridge) {
        vec3 motor = {0.0f, 0.0f, 0.0f};
        kitchen_fridge_motor(motor);
        audio_sound_set_position(sounds->fridge, motor);
    }
    sounds->inside = inside_target(eye);
    sounds->below = glm_smoothstep(BELOW_FROM, BELOW_TO, eye[1]);
}

float sounds_indoor_gain(const Sounds* sounds) {
    return STREET_GAIN + (1.0f - STREET_GAIN) * sounds->inside;
}

// Inside the mansion's great hall, open through both storeys.
static bool in_great_hall(float x, float z) {
    x -= MANSION_X;
    z -= MANSION_Z;
    return x > GREAT_X0 && x < GREAT_X1 && z > GREAT_Z0 && z < GREAT_Z1;
}

float sounds_gain_at(const Sounds* sounds, const vec3 listener, const vec3 source) {
    const bool hall = in_great_hall(listener[0], listener[2]);
    const bool apart = fabsf(listener[1] - source[1]) > STOREY_APART && !hall;
    return sounds_indoor_gain(sounds) * (apart ? OTHER_STOREY : 1.0f);
}

float sounds_overhead_gain(const Sounds* sounds) {
    return 1.0f - (1.0f - BELOW_OVERHEAD) * sounds->below;
}

void sounds_update(Sounds* sounds, const vec3 eye, float dt) {
    const float k = 1.0f - expf(-dt / INSIDE_SECONDS);
    sounds->inside += (inside_target(eye) - sounds->inside) * k;
    sounds->below += (glm_smoothstep(BELOW_FROM, BELOW_TO, eye[1]) - sounds->below) * k;
    const float wind = WIND_VOLUME * (1.0f - (1.0f - BELOW_WIND) * sounds->below);

    // The wind is all round, so its layers ride with the listener -- just
    // over its head, where they are inside the falloff's first metre and
    // pan to neither ear.
    vec3 over = {eye[0], eye[1] + 0.5f, eye[2]};
    if (sounds->wind_outside) {
        audio_sound_set_position(sounds->wind_outside, over);
        audio_sound_set_volume(sounds->wind_outside, wind * (1.0f - sounds->inside));
    }
    if (sounds->wind_inside) {
        audio_sound_set_position(sounds->wind_inside, over);
        audio_sound_set_volume(sounds->wind_inside, wind * sounds->inside);
    }
    if (sounds->fridge)
        audio_sound_set_volume(sounds->fridge, FRIDGE_VOLUME * sounds_indoor_gain(sounds) *
                                                   sounds_overhead_gain(sounds));
}
