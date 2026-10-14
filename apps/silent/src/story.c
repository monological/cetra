#include <stdio.h>
#include <string.h>

#include "rooms.h"
#include "story.h"

const char* const STORY_STAGE_NAMES[STORY_STAGE_COUNT] = {"locked", "haunted", "open"};

StoryStage story_stage_by_name(const char* name) {
    for (int i = 0; name && i < STORY_STAGE_COUNT; i++)
        if (!strcmp(name, STORY_STAGE_NAMES[i]))
            return (StoryStage)i;
    return STORY_NONE;
}

// A locked front door is yanked hard enough to rattle in its frame, the leaf pulled FRONT_PLAY
// metres off its stop at its latch edge.
#define FRONT_PLAY 0.01f

static void say(const Story* s, const char* what, double time) {
    if (s->trace)
        printf("story %s at %.3f\n", what, time);
}

// The front door, locked from the inside. The key opens it, once the player has the map off the
// fridge too, and the haunting ends as it does: false then, and the door is the player's.
static bool front_door(Story* s, Door* door, double time) {
    const Backpack* bp = s->house.backpack;
    if (backpack_holds(bp, ITEM_KEY)) {
        if (!backpack_holds(bp, ITEM_TOWN_MAP)) {
            hud_think(s->house.hud, "I think I saw a map in the kitchen. I might need it.");
            say(s, "front door, the key but no map", time);
            return true;
        }
        hud_think(s->house.hud, "It fits.");
        haunt_end(s->house.haunt);
        s->stage = STORY_OPEN;
        say(s, "front door unlocked", time);
        return false;
    }
    door_hand_try_begin(&s->try, s->house.sounds, door, time, true, FRONT_PLAY);
    if (!bp->taken) {
        hud_think(s->house.hud, "It's locked. From the inside? I need a key... and my bag.");
        say(s, "front door locked, no bag", time);
        return true;
    }
    hud_think(s->house.hud, "It's locked from the inside. I need to find a key.");
    say(s, "front door locked", time);
    return true;
}

void story_start(Story* s, const StoryHouse* house) {
    *s = (Story){.house = *house, .stage = STORY_LOCKED};
}

bool story_door(Story* s, Door* door, double time) {
    if (door == s->house.front && s->stage < STORY_OPEN)
        return front_door(s, door, time);
    return false;
}

// The haunting, begun as the key leaves the bench, and the cellar door slammed shut over the
// player's head down there; standing shut already, it does not move.
void story_key(Story* s, double time) {
    if (s->stage != STORY_LOCKED)
        return;
    haunt_begin(s->house.haunt, time, 0.0);
    if (s->house.cellar)
        door_slam(s->house.cellar, 0.0f);
    s->stage = STORY_HAUNTED;
    say(s, "key taken, the haunting begun", time);
}

void story_step(Story* s, double time) {
    door_hand_try_step(&s->try, s->house.sounds, time);
}

void story_settle(Story* s, const vec3 feet) {
    if (s->stage < STORY_OPEN && room_house(room_at(feet)) != ROOM_HOME)
        s->stage = STORY_OPEN;
    door_hand_try_stop(&s->try);
    if (s->stage < STORY_OPEN && s->house.front)
        door_set(s->house.front, 0.0f, 0.0f);
}
