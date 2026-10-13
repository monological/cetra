#ifndef _SILENT_STORY_H_
#define _SILENT_STORY_H_

#include <stdbool.h>

#include "backpack.h"
#include "door.h"
#include "door_try.h"
#include "haunt.h"
#include "hud.h"

/*
 * The player's house as a story (spec 13.53): its front door locked from the inside, and the way
 * out a key in the basement. Trying the front door with the backpack -- the flashlight that lets
 * the player down the cellar stair is in it -- begins the haunting; the cellar door is stuck until
 * then, and opening it ends the haunting; and the key on the basement's workbench, with the map
 * off the fridge, lets the player out.
 */

typedef enum StoryStage {
    STORY_LOCKED,  // the front door locked, the haunting not yet begun from it
    STORY_HAUNTED, // the haunting begun from the front door
    STORY_OVER,    // the haunting ended at the cellar door
    STORY_OPEN,    // the front door unlocked: an ordinary door from then on
} StoryStage;

// What the story takes hold of, borrowed; any may be NULL, and what it would do with it is not
// done.
typedef struct StoryHouse {
    AudioSystem* audio;
    Hud* hud;
    Backpack* backpack;
    Haunt* haunt;
    Door* front;
    Door* cellar;
} StoryHouse;

typedef struct Story {
    StoryHouse house;
    StoryStage stage;
    DoorTryTakes takes;
    DoorTry try; // the player's try at a door that will not open, while it lasts
    double last; // the sim time the last step reached
    int tries;   // the tries so far, which choose each one's knob take
    bool trace;  // say each turn of the story as it comes
} Story;

void story_start(Story* s, const StoryHouse* house);
// The action key on `door` at sim time `time`: true when the story took the press and the door
// is to stay as it is, false when the door is the player's to open or shut.
bool story_door(Story* s, Door* door, double time);
// Per fixed step, after the doors have moved: the player's try under way.
void story_step(Story* s, double time);

#endif // _SILENT_STORY_H_
