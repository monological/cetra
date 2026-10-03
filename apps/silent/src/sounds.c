#include <math.h>
#include <stdio.h>

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
 * 1 well inside the house's footprint, 0 well outside, blended over
 * WALL_BLEND across its walls. The front doorway is an open hole, so a
 * listener standing in it is half outside, which is right.
 */
static float inside_target(const vec3 eye) {
    return glm_smoothstep(0.5f * WALL_BLEND, -0.5f * WALL_BLEND, house_outside_distance(eye));
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
}

float sounds_indoor_gain(const Sounds* sounds) {
    return STREET_GAIN + (1.0f - STREET_GAIN) * sounds->inside;
}

float sounds_gain_at(const Sounds* sounds, const vec3 listener, const vec3 source) {
    const bool hall = listener[0] > GREAT_X0 && listener[0] < GREAT_X1 && listener[2] > GREAT_Z0 &&
                      listener[2] < GREAT_Z1;
    const bool apart = fabsf(listener[1] - source[1]) > STOREY_APART && !hall;
    return sounds_indoor_gain(sounds) * (apart ? OTHER_STOREY : 1.0f);
}

void sounds_update(Sounds* sounds, const vec3 eye, float dt) {
    const float k = 1.0f - expf(-dt / INSIDE_SECONDS);
    sounds->inside += (inside_target(eye) - sounds->inside) * k;

    // The wind is all round, so its layers ride with the listener -- just
    // over its head, where they are inside the falloff's first metre and
    // pan to neither ear.
    vec3 over = {eye[0], eye[1] + 0.5f, eye[2]};
    if (sounds->wind_outside) {
        audio_sound_set_position(sounds->wind_outside, over);
        audio_sound_set_volume(sounds->wind_outside, WIND_VOLUME * (1.0f - sounds->inside));
    }
    if (sounds->wind_inside) {
        audio_sound_set_position(sounds->wind_inside, over);
        audio_sound_set_volume(sounds->wind_inside, WIND_VOLUME * sounds->inside);
    }
    if (sounds->fridge)
        audio_sound_set_volume(sounds->fridge, FRIDGE_VOLUME * sounds_indoor_gain(sounds));
}
