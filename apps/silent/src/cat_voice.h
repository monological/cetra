#ifndef _SILENT_CAT_VOICE_H_
#define _SILENT_CAT_VOICE_H_

#include <stdbool.h>

#include <cglm/cglm.h>

#include "cetra/game/audio.h"

#include "cat.h"

/*
 * What the cat sounds like (spec 13.17): its meows, trill and hiss, its paws and its landings,
 * each played from the clip events that mean them, at its head or its feet and riding it; and a
 * purr, a loop held at its chest, heard close up when it is settled and at ease with the
 * listener. Every recording is levelled alike by tools/fetch_sounds.py, so the volumes in
 * cat_voice.c are the whole statement of how loud each one is.
 */

typedef struct CatVoice {
    Cat* cat;
    AudioSystem* audio; // NULL without sound
    Sound* meow[3];
    Sound* trill;
    Sound* hiss;
    Sound* paw[4];
    Sound* land_hard;
    Sound* land_soft;
    Sound* purr; // the cat entity's AUDIO_SOURCE
    int meows;   // how many meows so far, which picks the next
    int paws;
    float purr_level;
    bool say; // cycle through every sound it makes, to hear them
    float say_in;
    int say_next;
} CatVoice;

// Load the sounds, hang the purr on the cat, and listen to its clips' events. Nothing happens
// without `audio` or a cat. The voice must outlive the cat's animator.
void cat_voice_start(CatVoice* voice, Cat* cat, AudioSystem* audio, bool say);

// Once a frame: the purr, which wants a listener near, slow, and trusted (`at_ease`). Where the
// listener stands in the house is the audio zones' business.
void cat_voice_update(CatVoice* voice, const vec3 listener, bool at_ease, float dt);

#endif // _SILENT_CAT_VOICE_H_
