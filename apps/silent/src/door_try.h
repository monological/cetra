#ifndef _SILENT_DOOR_TRY_H_
#define _SILENT_DOOR_TRY_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/game/audio.h"

#include "door.h"
#include "door_takes.h"

/*
 * A door tried and not opened (specs 13.52 and 13.53): its knob turned and its leaf pulled against
 * the latch, which shakes in its frame on the recordings' own hits. The haunting tries the cellar
 * door from the stair over and over; a player tries a locked or stuck door once a press.
 */

// How far off its stop the leaf is pulled around the takes' hits at `u`, 0..1, for `take` started
// at `at`: each hit pulled out over the 50 ms before it and let fall over the 90 ms after, as
// strongly as the hit, the largest of them.
float door_take_pull(const DoorTake* take, double at, double u);

// The takes a try is heard by: the knob's and the rattle's, decoded once and played as voices.
// Each NULL without audio.
typedef struct DoorTryTakes {
    AudioSystem* audio;
    Sound* knob[DOOR_KNOB_TAKES];
    Sound* rattle;
} DoorTryTakes;

void door_try_takes_load(DoorTryTakes* takes, AudioSystem* audio);

// One try by hand: the door (NULL when none is under way), when it began, its knob's take, whether
// the rattle follows the knob a moment in, how far the leaf is pulled at its latch edge at most,
// metres, and how loud.
typedef struct DoorTry {
    Door* door;
    double at;
    int knob;
    bool shake;
    float play;
    float volume;
} DoorTry;

// Tries `door` from `time`: the knob's take `knob` heard now, the rattle after it when `shake`.
void door_try_begin(DoorTry* t, const DoorTryTakes* takes, Door* door, double time, int knob,
                    bool shake, float play, float volume);
// Per fixed step, after the doors have moved: the rattle heard as its time comes, and the leaf
// placed off its stop; and once the try is over, the leaf back on it.
void door_try_step(DoorTry* t, const DoorTryTakes* takes, double was, double now);

#endif // _SILENT_DOOR_TRY_H_
