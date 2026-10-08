#include <stdio.h>

#include "door.h"
#include "home.h"
#include "layout.h"
#include "sounds.h"

// Every loop is levelled alike by tools/fetch_sounds.py, so this is the whole statement of how
// loud the wind is.
#define WIND_VOLUME 0.6f

// What sound keeps crossing each kind of boundary: a house's outside walls, both houses alike,
// and a front door in them standing open; an inside wall with an open doorway through it; the
// floor and the door between the home's hall and its basement, shut and open; and the floor
// between the mansion's storeys. WALL_THROUGH is also how the wind and the rain tell which side
// of a house's walls the listener is on, so it is every outside wall's and no other boundary's.
#define WALL_THROUGH          0.3f
#define FRONT_DOOR_THROUGH    0.6f
#define DOORWAY_THROUGH       0.35f
#define BASEMENT_THROUGH      0.4f
#define BASEMENT_DOOR_THROUGH 0.8f
#define STOREY_THROUGH        0.5f

// Over every roof and the tower's spire: where a house's rooms stop going up.
#define ROOFS_Y 20.0f

// The wind's two layers ride this far over the eye: inside the falloff's first metre, panned to
// neither ear, and near enough that no boundary passes between them and the listener.
#define WIND_OVER 0.05f

enum {
    ROOM_WORLD, // the outdoors, outside every room
    ROOM_HOME,
    ROOM_LIVING,
    ROOM_KITCHEN,
    ROOM_BASEMENT,
    ROOM_MANSION,
    ROOM_UPSTAIRS,
    ROOMS
};

typedef struct Room {
    const char* name;
    bool mansion; // its boxes are in the mansion's plan, moved to where it stands
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
                   1,
                   {{{HOUSE_OUT_X0, BASEMENT_Y, HOUSE_OUT_Z0},
                     {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}}}},
    [ROOM_LIVING] = {"living_room",
                     false,
                     1,
                     {{{LIVING_IN_X0, SUBFLOOR_Y0, LIVING_IN_Z0},
                       {LIVING_IN_X1, CEIL_Y, LIVING_IN_Z1}}}},
    [ROOM_KITCHEN] = {"kitchen",
                      false,
                      1,
                      {{{KITCHEN_X0, SUBFLOOR_Y0, KITCHEN_Z0}, {KITCHEN_X1, CEIL_Y, KITCHEN_Z1}}}},
    [ROOM_BASEMENT] = {"basement",
                       false,
                       2,
                       {{{CELLAR_X0, BASEMENT_Y, CELLAR_Z0}, {CELLAR_X1, SUBFLOOR_Y0, CELLAR_Z1}},
                        {{CELLAR_X0, SUBFLOOR_Y0, STAIRWELL_Z0},
                         {HALL_OUT_X0, CEIL_Y, CELLAR_Z1}}}},
    [ROOM_MANSION] = {"mansion",
                      true,
                      2,
                      {{{HOUSE_OUT_X0, 0.0f, HOUSE_OUT_Z0}, {HOUSE_OUT_X1, ROOFS_Y, HOUSE_OUT_Z1}},
                       {{TOWER_X - TOWER_OUTER, 0.0f, TOWER_Z - TOWER_OUTER},
                        {TOWER_X + TOWER_OUTER, ROOFS_Y, TOWER_Z + TOWER_OUTER}}}},
    [ROOM_UPSTAIRS] = {"mansion_up",
                       true,
                       2,
                       {{{HOUSE_OUT_X0, CEIL_Y, HOUSE_OUT_Z0},
                         {HOUSE_OUT_X1, ROOFS_Y, KITCHEN_BACK_Z}},
                        {{TOWER_X - TOWER_OUTER, CEIL_Y, TOWER_Z - TOWER_OUTER},
                         {TOWER_X + TOWER_OUTER, ROOFS_Y, TOWER_Z + TOWER_OUTER}}}},
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
        AABB boxes[2];
        for (int i = 0; i < room->box_count; i++) {
            boxes[i] = room->boxes[i];
            if (room->mansion) {
                mansion_at(room->boxes[i].min, boxes[i].min);
                mansion_at(room->boxes[i].max, boxes[i].max);
            }
        }
        zone[r] = audio_zone_add(audio, &(AudioZoneDesc){room->name, boxes, room->box_count});
    }
    for (int i = 0; i < LINK_COUNT; i++) {
        const RoomLink* l = &LINKS[i];
        const AudioZoneLink link = audio_zone_link(audio, zone[l->a], zone[l->b], l->shut);
        if (l->door != SOUNDS_DOOR_NONE)
            sounds->door_links[l->door] = link;
    }
    sounds->wind_outside = sounds_loop(audio, "assets/audio/silent/wind_outside.flac");
    sounds->wind_inside = sounds_loop(audio, "assets/audio/silent/wind_inside.flac");
}

void sounds_update(Sounds* sounds, const vec3 eye) {
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

    // One curve of what the outdoors reaches the listener with, broken at a house's walls: down
    // to WALL_THROUGH the walls are all that stands between, and how far its doors stand open is
    // how much of the full wind comes in; below it something more does -- a floor -- and only
    // the level falls.
    const float outdoors = audio_zone_heard(audio, AUDIO_ZONE_WORLD);
    const float indoors = glm_percentc(1.0f, WALL_THROUGH, outdoors);
    sounds->past_walls = glm_percentc(0.0f, WALL_THROUGH, outdoors);
    vec3 over = {eye[0], eye[1] + WIND_OVER, eye[2]};
    if (sounds->wind_outside) {
        audio_sound_set_position(sounds->wind_outside, over);
        audio_sound_set_volume(sounds->wind_outside, WIND_VOLUME * (1.0f - indoors));
    }
    if (sounds->wind_inside) {
        audio_sound_set_position(sounds->wind_inside, over);
        audio_sound_set_volume(sounds->wind_inside, WIND_VOLUME * indoors * sounds->past_walls);
    }
}
