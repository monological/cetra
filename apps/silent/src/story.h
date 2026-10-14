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
 * The player's house as a story (specs 13.53 and 13.54): its front door locked from the inside,
 * and the way out a key on the basement's workbench, down the cellar stair the backpack's
 * flashlight lets the player down. Taking the key begins the haunting, and the cellar door slams
 * shut above them; the key, with the map off the fridge, opens the front door, and that ends it.
 */

typedef enum StoryStage {
    STORY_NONE = -1,
    STORY_LOCKED,  // the front door locked, the key not yet taken
    STORY_HAUNTED, // the key taken, and the haunting begun with it
    STORY_OPEN,    // the front door unlocked: an ordinary door from then on
    STORY_STAGE_COUNT
} StoryStage;

// Each stage as a save and a command line name it: "locked", "haunted", "open".
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
// is not to be toggled, false when the door is the player's to open or shut.
bool story_door(Story* s, Door* door, double time);
// The key taken off the workbench at sim time `time`: from a locked house, the haunting.
void story_key(Story* s, double time);
// Per fixed step at sim time `time`, after the doors have moved: the player's try under way.
void story_step(Story* s, double time);
/*
 * The house put as its stage says (a load, or the command line), with the player's feet at `feet`:
 * anywhere but in the house the stage is STORY_OPEN whatever it was, since they are out of it.
 * Below that the front door is shut. A try under way is dropped.
 */
void story_settle(Story* s, const vec3 feet);

#endif // _SILENT_STORY_H_
