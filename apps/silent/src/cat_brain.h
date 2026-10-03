#ifndef _SILENT_CAT_BRAIN_H_
#define _SILENT_CAT_BRAIN_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "cetra/game/brain.h"
#include "cetra/game/game.h"

#include "cat.h"
#include "player.h"

/*
 * What the cat wants (spec 13.17): an aloof watcher. It sleeps, loafs, wanders, watches the
 * clock and the rain, turns its head to watch the player when the player is near and in
 * sight, moves off if rushed, and now and then follows at a distance. The choosing is the
 * engine's brain; what each activity means, and what the cat sees and wants, is here.
 */

typedef enum {
    CAT_GAZE_NONE,
    CAT_GAZE_PLAYER,
    CAT_GAZE_CLOCK,
    CAT_GAZE_WINDOW,
} CatGaze;

typedef struct CatMind {
    Cat* cat;
    Brain* brain; // the cat entity's; NULL when there is none
    int first;    // the activity to start once the cat is in the house, -1 to choose
    Game* game;
    const Player* player;
    bool blind;
    float rain; // 0..1, how hard it rains

    // Needs, 0..1.
    float energy;
    float boredom;
    float alarm;
    float affinity;
    float since[CAT_PLACE_COUNT]; // seconds since it was last at each place

    // What it senses of the player.
    float sense_in; // seconds to the next look
    bool sees, hears;
    float unseen;                // seconds out of sight
    vec3 player_eye;             // the body's, wherever the camera is
    float player_speed, closing; // over the ground; closing is how fast they come on
    float distance;              // across the ground, and half of any drop between
    bool startled;               // a startle is due
    float still_look;            // seconds of being looked at by a player standing still

    // The activity under way.
    int spot;      // the place it is at or going to, -1 for none
    float hold;    // seconds to stay once there
    float held;    // seconds there so far
    float give_up; // seconds left before it stops trying to get there
    bool asleep;
    bool want_stretch;
    bool groomed;
    bool follow_on;
    float follow_roll_in;
    float retarget_in;

    // The head and the face.
    CatGaze gaze;
    vec3 gaze_at;
    float glance_in, glance_left;
    float blink_in, flick_in, meow_in;
} CatMind;

// The cat's mind, on the cat's entity, seeded: the same seed makes the same cat. `first`
// names an activity to start with, or NULL to choose. False when there is no cat.
bool cat_mind_create(CatMind* mind, Cat* cat, Game* game, const Player* player, uint32_t seed,
                     bool blind, const char* first);

// Each fixed step, before the brains: what it senses and how its needs change. `rain` is how
// hard it is raining, 0..1.
void cat_mind_sense(CatMind* mind, float rain, float dt);

// Once a frame before the cat's update: where it looks.
void cat_mind_frame(CatMind* mind, double time);

// Whether it would purr for the player now: someone it likes, not hurrying. With no mind it has
// no one to mistrust.
bool cat_mind_at_ease(const CatMind* mind);

// One line of what it wants, for --trace-cat.
void cat_mind_trace(const CatMind* mind);

// The activities' names, comma-separated, for the usage line.
const char* cat_mind_activities(void);

#endif // _SILENT_CAT_BRAIN_H_
