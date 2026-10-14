#include <math.h>
#include <stdio.h>

#include "door.h"
#include "rooms.h"
#include "sounds.h"

// Every loop is levelled alike by tools/fetch_sounds.py, so this is the whole statement of how
// loud the wind is.
#define WIND_VOLUME 0.3f

// What sound keeps crossing each kind of boundary: a house's outside walls, both houses alike,
// and a front door in them standing open; an inside wall with an open doorway through it; the
// floor and the door between the home's hall and its basement, shut and open; and the floor
// between the mansion's storeys. WALL_THROUGH is also where the wind is all muffled: the
// outdoors coming in at that, against the rooms it comes in through, is a house shut up.
#define WALL_THROUGH          0.3f
#define FRONT_DOOR_THROUGH    0.6f
#define DOORWAY_THROUGH       0.35f
#define BASEMENT_THROUGH      0.4f
#define BASEMENT_DOOR_THROUGH 0.8f
#define STOREY_THROUGH        0.5f

_Static_assert(ROOM_BOXES <= AUDIO_ZONE_BOXES, "every room's boxes fit one zone");

typedef struct RoomLink {
    int a, b;         // rooms
    float shut, open; // `through` with its door shut and open; one with no door is always shut
    SoundsDoor door;  // SOUNDS_DOOR_NONE for a boundary nothing opens
} RoomLink;

// Every room with an outside wall reaches the outdoors through it, and a room over the basement
// reaches it through its own floor rather than by way of a neighbour: a room is the outdoors'
// neighbour and the next room's at once.
static const RoomLink LINKS[] = {
    {ROOM_HOME, ROOM_WORLD, WALL_THROUGH, FRONT_DOOR_THROUGH, SOUNDS_DOOR_HOME},
    {ROOM_BASEMENT, ROOM_HOME, BASEMENT_THROUGH, BASEMENT_DOOR_THROUGH, SOUNDS_DOOR_BASEMENT},
    {ROOM_LIVING, ROOM_HOME, DOORWAY_THROUGH},
    {ROOM_LIVING, ROOM_WORLD, WALL_THROUGH},
    {ROOM_LIVING, ROOM_BASEMENT, BASEMENT_THROUGH},
    {ROOM_KITCHEN, ROOM_HOME, DOORWAY_THROUGH},
    {ROOM_KITCHEN, ROOM_WORLD, WALL_THROUGH},
    {ROOM_KITCHEN, ROOM_BASEMENT, BASEMENT_THROUGH},
    {ROOM_MANSION, ROOM_WORLD, WALL_THROUGH, FRONT_DOOR_THROUGH, SOUNDS_DOOR_MANSION},
    {ROOM_UPSTAIRS, ROOM_WORLD, WALL_THROUGH},
    {ROOM_UPSTAIRS, ROOM_MANSION, STOREY_THROUGH},
    {ROOM_CABIN, ROOM_WORLD, WALL_THROUGH, FRONT_DOOR_THROUGH, SOUNDS_DOOR_CABIN},
};
#define LINK_COUNT ((int)(sizeof(LINKS) / sizeof(LINKS[0])))

Sound* sounds_loop(AudioSystem* audio, const char* path, AudioBus bus) {
    if (!audio)
        return NULL;
    Sound* s = audio_sound_from_file(audio, path, bus);
    if (!s) {
        fprintf(stderr, "silent: cannot load %s\n", path);
        return NULL;
    }
    audio_sound_set_looping(s, true);
    audio_sound_set_volume(s, 0.0f);
    audio_sound_play(s);
    return s;
}

void sounds_start(Sounds* sounds, AudioSystem* audio, const struct Door* const* doors) {
    *sounds = (Sounds){.audio = audio, .past_walls = 1.0f};
    for (int d = 0; d < SOUNDS_DOORS; d++) {
        sounds->doors[d] = doors[d];
        sounds->door_links[d] = AUDIO_ZONE_NO_LINK;
    }
    if (!audio)
        return;
    AudioZone zone[ROOMS] = {[ROOM_WORLD] = AUDIO_ZONE_WORLD};
    for (int r = ROOM_WORLD + 1; r < ROOMS; r++) {
        AABB boxes[ROOM_BOXES];
        const int n = room_boxes((RoomId)r, boxes);
        zone[r] = audio_zone_add(audio, &(AudioZoneDesc){room_name((RoomId)r), boxes, n});
    }
    bool outer[ROOMS] = {false};
    for (int i = 0; i < LINK_COUNT; i++) {
        const RoomLink* l = &LINKS[i];
        const AudioZoneLink link = audio_zone_link(audio, zone[l->a], zone[l->b], l->shut);
        if (l->door != SOUNDS_DOOR_NONE)
            sounds->door_links[l->door] = link;
        outer[l->a] |= l->b == ROOM_WORLD;
        outer[l->b] |= l->a == ROOM_WORLD;
    }
    for (int r = ROOM_WORLD + 1; r < ROOMS; r++)
        if (outer[r])
            sounds->outer[sounds->outer_count++] = zone[r];
    sounds->wind_outside =
        sounds_loop(audio, "assets/audio/silent/wind_outside.flac", AUDIO_BUS_SFX);
    sounds->wind_inside = sounds_loop(audio, "assets/audio/silent/wind_inside.flac", AUDIO_BUS_SFX);
}

void sounds_update(Sounds* sounds) {
    AudioSystem* audio = sounds->audio;
    if (!audio)
        return;
    for (int i = 0; i < LINK_COUNT; i++) {
        const RoomLink* l = &LINKS[i];
        if (l->door == SOUNDS_DOOR_NONE)
            continue;
        const struct Door* door = sounds->doors[l->door];
        audio_zone_link_set(audio, sounds->door_links[l->door],
                            glm_lerp(l->shut, l->open, door ? door->travel : 0.0f));
    }

    // The outdoors comes into a house through the rooms with an outside wall. How well the
    // listener hears the best of them is the wind's LEVEL -- 1 in any of them, a floor's worth
    // less under them -- and a front door has no part in it. How much of the outdoors comes in
    // with them is how far the doors stand open, which is its MIX: at WALL_THROUGH, all muffled.
    const float outdoors = audio_zone_heard(audio, AUDIO_ZONE_WORLD);
    float level = outdoors;
    for (int i = 0; i < sounds->outer_count; i++)
        level = fmaxf(level, audio_zone_heard(audio, sounds->outer[i]));
    const float indoors = glm_percentc(1.0f, WALL_THROUGH, level > 0.0f ? outdoors / level : 0.0f);
    sounds->past_walls = level;
    if (sounds->wind_outside)
        audio_sound_set_volume(sounds->wind_outside, WIND_VOLUME * (1.0f - indoors) * level);
    if (sounds->wind_inside)
        audio_sound_set_volume(sounds->wind_inside, WIND_VOLUME * indoors * level);
}
