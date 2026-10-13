#ifndef _SILENT_STORY_H_
#define _SILENT_STORY_H_

#include <stdbool.h>
#include <cglm/cglm.h>

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
    STORY_NONE = -1,
    STORY_LOCKED,  // the front door locked, the haunting not yet begun from it
    STORY_HAUNTED, // the haunting begun from the front door
    STORY_OVER,    // the haunting ended at the cellar door
    STORY_OPEN,    // the front door unlocked: an ordinary door from then on
    STORY_STAGE_COUNT
} StoryStage;

// Each stage as a save and a command line name it: "locked", "haunted", "over", "open".
extern const char* const STORY_STAGE_NAMES[STORY_STAGE_COUNT];
// The stage `name` names, or STORY_NONE.
StoryStage story_stage_by_name(const char* name);

// What the story takes hold of, borrowed. The doors are NULL when they were not hung, and what
// the story would do at one is not done; the rest are required.
typedef struct StoryHouse {
    const DoorSounds* sounds;
    Hud* hud;
    Backpack* backpack;
    Haunt* haunt;
    Door* front;
    Door* cellar;
} StoryHouse;

typedef struct Story {
    StoryHouse house;
    StoryStage stage;
    DoorHandTry try; // the player's try at a door that will not open
    bool trace;      // say each turn of the story as it comes
} Story;

// The story at its start.
void story_start(Story* s, const StoryHouse* house);
// The action key on `door` at sim time `time`: true when the story took the press and the door
// is to stay as it is, false when the door is the player's to open or shut.
bool story_door(Story* s, Door* door, double time);
// Per fixed step at sim time `time`, after the doors have moved: the player's try under way.
void story_step(Story* s, double time);
/*
 * The house put as its stage says (a load, or the command line), with the player's feet at `feet`:
 * anywhere but in the house the stage is STORY_OPEN whatever it was, since they are out of it.
 * Below that the front door is shut. A try under way is dropped.
 */
void story_settle(Story* s, const vec3 feet);

#endif // _SILENT_STORY_H_
