#ifndef _SILENT_STORY_H_
#define _SILENT_STORY_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/scene.h"

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

// What the story takes hold of, borrowed; any may be NULL, and what it would do with it is not
// done.
typedef struct StoryHouse {
    Engine* engine;
    Scene* scene;
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
    Sound* slam; // the cellar door flung against its stop; NULL without audio
    bool slamming;
    SceneNode* key; // on the basement's workbench, and kept off the graph once it is taken
    vec3 key_at;    // its middle, which the player reaches for
    double last;    // the sim time the last step reached
    int tries;      // the tries so far, which choose each one's knob take
    bool rehaunt;   // put at a haunted stage: the haunting begins again once the game is played
    bool trace;     // say each turn of the story as it comes
} Story;

// The story at its start, and the key lying on the basement's workbench.
void story_start(Story* s, const StoryHouse* house);
void story_free(Story* s);
// The key on the workbench, or off it and kept: it is off it exactly while the backpack holds it.
void story_key_on_bench(Story* s, bool on);
// The front door's key, a heavy brass bit key along f's d and centred on its origin: `flat` lying
// on its side on y, or standing on its edge, its bit up, about y.
void story_key_model(Kit* kit, const KitFrame* f, float y, bool flat);
// The action key on `door` at sim time `time`: true when the story took the press and the door
// is to stay as it is, false when the door is the player's to open or shut.
bool story_door(Story* s, Door* door, double time);
// Per fixed step, after the doors have moved: the player's try under way.
void story_step(Story* s, double time);
/*
 * The story put at `stage` (a load, or the command line), with the player's feet at `feet`:
 * anywhere but in the house it is STORY_OPEN whatever the stage says, since they are out of it.
 * Below that the front door is shut. The key lies on the workbench unless the backpack holds it. A
 * try under way is dropped. Haunted, the haunting begins again at story_play.
 */
void story_restore(Story* s, StoryStage stage, const vec3 feet);
// Per frame while the game is played, at sim time `time`: the haunting a restore left begun.
void story_play(Story* s, double time);

#endif // _SILENT_STORY_H_
