#ifndef _SILENT_DOOR_TRY_H_
#define _SILENT_DOOR_TRY_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/game/audio.h"

#include "door.h"
#include "door_takes.h"

/*
 * A door tried and not opened (specs 13.52 and 13.53): its knob turned and its leaf pulled against
 * the latch, which shakes in its frame on the recordings' own hits. And what the doors sound like.
 */

// The doors' sounds, decoded once: the knob's takes and the rattle's, a door slammed against its
// stop, and one creaking. Each NULL without audio.
typedef struct DoorSounds {
    AudioSystem* audio;
    Sound* knob[DOOR_KNOB_TAKES];
    Sound* rattle;
    Sound* slam;
    Sound* creak;
} DoorSounds;

// A slam against its stop, against the one-shots tools/fetch_sounds.py levels alike.
#define DOOR_SLAM_VOLUME 2.0f

void door_sounds_load(DoorSounds* s, AudioSystem* audio);
// A fire-and-forget copy of `sound`, one of `s`'s, heard at `door`'s leaf.
void door_hear(const DoorSounds* s, const Sound* sound, const Door* door, float volume);

#define DOOR_TRY_TURNS 4 // the knob's turns in a try, at most
// The door is shaken from this long after the knob is first turned.
#define DOOR_RATTLE_LAG 0.3

// A try, in seconds: when it starts; the knob takes it turns, one after another, and when each
// starts from the try's start; how many of the rattle's takes it shakes the door through, one
// after another from DOOR_RATTLE_LAG in, 0 for the knob alone; and when it is all over.
typedef struct DoorTry {
    double at;
    int turns;
    int knob[DOOR_TRY_TURNS];
    double turn_at[DOOR_TRY_TURNS];
    int shakes;
    double end;
} DoorTry;

// When shake `j` of a try starts, from the try's start.
double door_try_shake_at(int j);
// How far off its stop `t` pulls the leaf at `u`, 0..1: the largest pull of any of its takes'
// hits, the knob's half the rattle's.
float door_try_pull(const DoorTry* t, double u);

// A player's try at a door that will not open (NULL when none is under way): the knob turned once
// and, when it is shaken, the door after it, the leaf pulled at most `play` metres off its stop at
// its latch edge. Its count of tries chooses each one's knob take.
typedef struct DoorHandTry {
    Door* door;
    DoorTry try;
    float play;
    double last; // the sim time the last step reached
    int count;
} DoorHandTry;

// Tries `door` from `time`, the knob heard now.
void door_hand_try_begin(DoorHandTry* t, const DoorSounds* s, Door* door, double time, bool shake,
                         float play);
// Per fixed step at sim time `now`, after the doors have moved: the rattle heard as its time comes
// and the leaf placed off its stop, or back on it once the try is over.
void door_hand_try_step(DoorHandTry* t, const DoorSounds* s, double now);
// The try dropped, the leaf back on its stop.
void door_hand_try_stop(DoorHandTry* t);

#endif // _SILENT_DOOR_TRY_H_
