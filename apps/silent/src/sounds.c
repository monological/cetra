#include <math.h>
#include <stdio.h>

#include "door.h"
#include "home.h"
#include "layout.h"
#include "sounds.h"
#include "tower.h"

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

// Over every roof and the tower's spire: where a house's rooms stop going up.
#define ROOFS_Y 20.0f

enum {
    ROOM_WORLD, // the outdoors, outside every room
    ROOM_HOME,
    ROOM_LIVING,
    ROOM_KITCHEN,
    ROOM_BASEMENT,
    ROOM_MANSION,
    ROOM_UPSTAIRS,
    ROOM_CABIN,
    ROOMS
};

typedef struct Room {
    const char* name;
    bool mansion; // its boxes are in the mansion's plan, moved to where it stands
    bool tower;   // and the tower's octagon with them, as high as the first box
    int box_count;
    AABB boxes[2];
} Room;

/*
 * The rooms, in the order they are added, since a room added later carves itself out of one
 * round it. The home over its footprint, which is its hall and everything shut; the living room
 * and the kitchen, each off the hall through an open doorway; and its basement -- the cellar
 * under it all and the flight down behind its door, so the door is the basement's boundary. The
 * mansion over its footprint and its tower, and its upper storey -- the rooms over the front and
 * the study's bay in the tower -- carved out of it; the gallery stays the great hall's, open to
 * it.
 */
static const Room ROOM_TABLE[ROOMS] = {
    [ROOM_HOME] = {"home",
                   false,
                   false,
                   1,
                   {{{HOUSE_OUT_X0, BASEMENT_Y, HOUSE_OUT_Z0},
                     {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}}}},
    [ROOM_LIVING] = {"living_room",
                     false,
                     false,
                     1,
                     {{{LIVING_IN_X0, SUBFLOOR_Y0, LIVING_IN_Z0},
                       {LIVING_IN_X1, CEIL_Y, LIVING_IN_Z1}}}},
    [ROOM_KITCHEN] = {"kitchen",
                      false,
                      false,
                      1,
                      {{{KITCHEN_X0, SUBFLOOR_Y0, KITCHEN_Z0}, {KITCHEN_X1, CEIL_Y, KITCHEN_Z1}}}},
    [ROOM_BASEMENT] = {"basement",
                       false,
                       false,
                       2,
                       {{{CELLAR_X0, BASEMENT_Y, CELLAR_Z0}, {CELLAR_X1, SUBFLOOR_Y0, CELLAR_Z1}},
                        {{CELLAR_X0, SUBFLOOR_Y0, STAIRWELL_Z0},
                         {HALL_OUT_X0, CEIL_Y, CELLAR_Z1}}}},
    [ROOM_MANSION] = {"mansion",
                      true,
                      true,
                      1,
                      {{{HOUSE_OUT_X0, 0.0f, HOUSE_OUT_Z0},
                        {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}}}},
    [ROOM_UPSTAIRS] = {"mansion_up",
                       true,
                       true,
                       1,
                       {{{HOUSE_OUT_X0, CEIL_Y, HOUSE_OUT_Z0},
                         {HOUSE_OUT_X1, ROOFS_Y, KITCHEN_BACK_Z}}}},
    // The cabin by the lake (spec 13.41), its one room to the roof.
    [ROOM_CABIN] = {"cabin",
                    false,
                    false,
                    1,
                    {{{CABIN_X0, CABIN_FLOOR_Y - 0.5f, CABIN_Z0},
                      {CABIN_X1, CABIN_RIDGE_Y + 0.5f, CABIN_Z1}}}},
};

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
        const Room* room = &ROOM_TABLE[r];
        AABB boxes[AUDIO_ZONE_BOXES];
        int n = 0;
        for (int i = 0; i < room->box_count; i++)
            boxes[n++] = room->boxes[i];
        if (room->tower)
            n += tower_boxes(room->boxes[0].min[1], room->boxes[0].max[1], &boxes[n]);
        for (int i = 0; room->mansion && i < n; i++) {
            mansion_at(boxes[i].min, boxes[i].min);
            mansion_at(boxes[i].max, boxes[i].max);
        }
        zone[r] = audio_zone_add(audio, &(AudioZoneDesc){room->name, boxes, n});
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
    sounds->wind_outside = sounds_loop(audio, "assets/audio/silent/wind_outside.flac");
    sounds->wind_inside = sounds_loop(audio, "assets/audio/silent/wind_inside.flac");
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
