#include <math.h>
#include <stdio.h>

#include "home.h"
#include "kitchen.h"
#include "layout.h"
#include "sounds.h"

// Every loop is levelled alike by tools/fetch_sounds.py, so these are the
// whole statement of how loud each one is.
#define FRIDGE_VOLUME 0.15f
#define WIND_VOLUME   0.6f

// What sound keeps crossing each kind of boundary: a house's outside walls, both houses alike,
// and a front door in them standing open; an inside wall with an open doorway through it; the
// floor and the door between the home's hall and its basement, shut and open; and the floor
// between the mansion's storeys.
#define WALL_THROUGH          0.3f
#define FRONT_DOOR_THROUGH    0.6f
#define DOORWAY_THROUGH       0.35f
#define BASEMENT_THROUGH      0.4f
#define BASEMENT_DOOR_THROUGH 0.8f
#define STOREY_THROUGH        0.5f

// Over every roof and the tower's spire: where a house's rooms stop going up.
#define ROOFS_Y 20.0f

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

// A box of the mansion's plan, where the mansion stands.
static AABB mansion_box(float x0, float y0, float z0, float x1, float y1, float z1) {
    AABB box = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    mansion_at((float[3]){x0, y0, z0}, box.min);
    mansion_at((float[3]){x1, y1, z1}, box.max);
    return box;
}

/*
 * The rooms. The home over its footprint, which is its hall and everything shut; the living room
 * and the kitchen, each off the hall through an open doorway; and its basement -- the cellar
 * under it all and the flight down behind its door, so the door is the basement's boundary --
 * each carved out of it. The mansion over its footprint and its tower, and its upper storey --
 * the rooms over the front and the study's bay in the tower -- carved out of it; the gallery stays
 * the great hall's, open to it. Every room with an outside wall reaches the outdoors through it,
 * and a room over the basement reaches it through its own floor, rather than by way of a
 * neighbour: a room is the outdoors' neighbour and the next room's at once.
 */
static void rooms(Sounds* sounds, AudioSystem* audio) {
    const AABB home = {{DIG_X0, BASEMENT_Y, DIG_Z0}, {DIG_X1, ROOFS_Y, DIG_Z1}};
    const AABB living = {{LIVING_IN_X0, SUBFLOOR_Y0, LIVING_IN_Z0},
                         {LIVING_IN_X1, CEIL_Y, LIVING_IN_Z1}};
    const AABB kitchen = {{HALL_X1 + 0.5f * INT_WALL, SUBFLOOR_Y0, BAND_Z0},
                          {HOUSE_X1 - 0.5f * EXT_WALL, CEIL_Y, BAND_Z1}};
    const AABB cellar[2] = {
        {{CELLAR_X0, BASEMENT_Y, CELLAR_Z0}, {CELLAR_X1, SUBFLOOR_Y0, CELLAR_Z1}},
        {{CELLAR_X0, SUBFLOOR_Y0, STAIRWELL_Z0}, {HALL_X0 - 0.5f * INT_WALL, CEIL_Y, CELLAR_Z1}}};
    const float wall = 0.5f * EXT_WALL, t = TOWER_OUTER;
    const AABB mansion[2] = {
        mansion_box(HOUSE_X0 - wall, 0.0f, HOUSE_FRONT_Z - wall, HOUSE_X1 + wall, ROOFS_Y,
                    HOUSE_BACK_Z + wall),
        mansion_box(TOWER_X - t, 0.0f, TOWER_Z - t, TOWER_X + t, ROOFS_Y, TOWER_Z + t)};
    const AABB upstairs[2] = {
        mansion_box(HOUSE_X0 - wall, CEIL_Y, HOUSE_FRONT_Z - wall, HOUSE_X1 + wall, ROOFS_Y,
                    KITCHEN_BACK_Z),
        mansion_box(TOWER_X - t, CEIL_Y, TOWER_Z - t, TOWER_X + t, ROOFS_Y, TOWER_Z + t)};
    const AudioZone h = audio_zone_add(audio, &(AudioZoneDesc){"home", &home, 1});
    const AudioZone l = audio_zone_add(audio, &(AudioZoneDesc){"living_room", &living, 1});
    const AudioZone k = audio_zone_add(audio, &(AudioZoneDesc){"kitchen", &kitchen, 1});
    const AudioZone b = audio_zone_add(audio, &(AudioZoneDesc){"basement", cellar, 2});
    const AudioZone m = audio_zone_add(audio, &(AudioZoneDesc){"mansion", mansion, 2});
    const AudioZone u = audio_zone_add(audio, &(AudioZoneDesc){"mansion_up", upstairs, 2});
    sounds->home_door = audio_zone_link(audio, h, AUDIO_ZONE_WORLD, WALL_THROUGH);
    sounds->basement_door = audio_zone_link(audio, b, h, BASEMENT_THROUGH);
    const AudioZone off_hall[2] = {l, k};
    for (int i = 0; i < 2; i++) {
        audio_zone_link(audio, off_hall[i], h, DOORWAY_THROUGH);
        audio_zone_link(audio, off_hall[i], AUDIO_ZONE_WORLD, WALL_THROUGH);
        audio_zone_link(audio, off_hall[i], b, BASEMENT_THROUGH);
    }
    sounds->mansion_door = audio_zone_link(audio, m, AUDIO_ZONE_WORLD, WALL_THROUGH);
    audio_zone_link(audio, u, AUDIO_ZONE_WORLD, WALL_THROUGH);
    audio_zone_link(audio, u, m, STOREY_THROUGH);
}

void sounds_start(Sounds* sounds, AudioSystem* audio) {
    *sounds = (Sounds){.audio = audio};
    if (!audio)
        return;
    rooms(sounds, audio);
    sounds->fridge = sounds_loop(audio, "assets/audio/silent/fridge_hum.flac");
    sounds->wind_outside = sounds_loop(audio, "assets/audio/silent/wind_outside.flac");
    sounds->wind_inside = sounds_loop(audio, "assets/audio/silent/wind_inside.flac");
    if (sounds->fridge) {
        vec3 motor = {0.0f, 0.0f, 0.0f};
        kitchen_fridge_motor(motor);
        audio_sound_set_position(sounds->fridge, motor);
        audio_sound_set_volume(sounds->fridge, FRIDGE_VOLUME);
    }
}

float sounds_past_walls(const Sounds* sounds) {
    if (!sounds->audio)
        return 1.0f;
    return glm_clamp(audio_zone_heard(sounds->audio, AUDIO_ZONE_WORLD) / WALL_THROUGH, 0.0f, 1.0f);
}

void sounds_update(Sounds* sounds, const vec3 eye, float home_door, float mansion_door,
                   float basement_door) {
    AudioSystem* audio = sounds->audio;
    if (!audio)
        return;
    audio_zone_link_set(audio, sounds->home_door,
                        glm_lerp(WALL_THROUGH, FRONT_DOOR_THROUGH, home_door));
    audio_zone_link_set(audio, sounds->mansion_door,
                        glm_lerp(WALL_THROUGH, FRONT_DOOR_THROUGH, mansion_door));
    audio_zone_link_set(audio, sounds->basement_door,
                        glm_lerp(BASEMENT_THROUGH, BASEMENT_DOOR_THROUGH, basement_door));

    // The wind is all round, so its layers ride with the listener -- just over its head, where
    // they are inside the falloff's first metre, pan to neither ear and are always in the
    // listener's own zone. Indoors is how far the outdoors has fallen to what a house's walls
    // let through, and the muffled layer is quieter again below those walls.
    const float outdoors = audio_zone_heard(audio, AUDIO_ZONE_WORLD);
    const float indoors = glm_clamp((1.0f - outdoors) / (1.0f - WALL_THROUGH), 0.0f, 1.0f);
    vec3 over = {eye[0], eye[1] + 0.5f, eye[2]};
    if (sounds->wind_outside) {
        audio_sound_set_position(sounds->wind_outside, over);
        audio_sound_set_volume(sounds->wind_outside, WIND_VOLUME * (1.0f - indoors));
    }
    if (sounds->wind_inside) {
        audio_sound_set_position(sounds->wind_inside, over);
        audio_sound_set_volume(sounds->wind_inside,
                               WIND_VOLUME * indoors * sounds_past_walls(sounds));
    }
}
