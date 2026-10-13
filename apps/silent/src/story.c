#include <stdio.h>

#include "save_game.h"
#include "story.h"

// A locked front door is yanked hard enough to rattle in its frame, the leaf pulled FRONT_PLAY
// metres off its stop at its latch edge; a stuck cellar door only gives a little. Both are heard
// from right beside them.
#define FRONT_PLAY  0.01f
#define CELLAR_PLAY 0.004f
#define TRY_VOLUME  1.2f

static void think(Story* s, const char* text) {
    if (s->house.hud)
        hud_think(s->house.hud, text);
}

static void say(const Story* s, const char* what, double time) {
    if (s->trace)
        printf("story %s at %.3f\n", what, time);
}

// The player's try at `door`, which will not open: its knob turned, and when `shake`, its leaf
// pulled against the latch.
static void try_door(Story* s, Door* door, double time, bool shake, float play) {
    door_try_begin(&s->try, &s->takes, door, time, s->tries++, shake, play, TRY_VOLUME);
}

static bool door_shut(const Door* door) {
    return door->travel <= 0.0f && door->want <= 0.0f;
}

// The front door, locked from the inside. With the backpack -- whose flashlight is what lets the
// player down the cellar stair to the key -- the first try begins the haunting.
static bool front_door(Story* s, Door* door, double time) {
    try_door(s, door, time, true, FRONT_PLAY);
    const bool bag = s->house.backpack && s->house.backpack->taken;
    if (!bag) {
        think(s, "It's locked. From the inside? I need a key... and my bag.");
        say(s, "front door locked, no bag", time);
        return true;
    }
    if (s->stage == STORY_LOCKED) {
        think(s, "It's locked from the inside. I need to find a key.");
        if (s->house.haunt)
            haunt_begin(s->house.haunt, time, 0.0);
        s->stage = STORY_HAUNTED;
        save_game_due();
        say(s, "front door locked, the haunting begun", time);
        return true;
    }
    think(s, "I need to find the key.");
    say(s, "front door locked", time);
    return true;
}

// The cellar door: stuck until the haunting has begun, so nobody reaches the key down there
// before it.
static bool cellar_door(Story* s, Door* door, double time) {
    const bool haunted = s->house.haunt && haunt_active(s->house.haunt);
    if (haunted || s->stage != STORY_LOCKED || !door_shut(door))
        return false;
    try_door(s, door, time, false, CELLAR_PLAY);
    think(s, "It won't open. It's stuck.");
    say(s, "cellar door stuck", time);
    return true;
}

void story_start(Story* s, const StoryHouse* house) {
    *s = (Story){.house = *house, .stage = STORY_LOCKED, .last = -1.0};
    door_try_takes_load(&s->takes, house->audio);
}

bool story_door(Story* s, Door* door, double time) {
    if (!door)
        return false;
    if (door == s->house.front && s->stage < STORY_OPEN)
        return front_door(s, door, time);
    if (door == s->house.cellar)
        return cellar_door(s, door, time);
    return false;
}

void story_step(Story* s, double time) {
    const double was = s->last < 0.0 ? time : s->last;
    s->last = time;
    door_try_step(&s->try, &s->takes, was, time);
}
