#ifndef _SILENT_RAIN_BED_H_
#define _SILENT_RAIN_BED_H_

#include "cetra/rain.h"
#include "cetra/shadow.h"
#include "cetra/game/audio.h"

/*
 * The rain, heard (spec 13.9): two noise beds, as loud as the rate. Pink noise is the patter of
 * the drops landing round the listener, and it is what a roof takes away; brown is the rumble
 * of the rain on everything, which a roof passes on as the drumming on it. Under cover the
 * patter falls and the rumble stays, which reads as muffled without a filter graph.
 *
 * Whether the listener is under cover is the rain's own occlusion map, read back at the head a
 * few frames late (shadow_rain_cover_ask) and eased, so walking in at the door is a fade.
 */
typedef struct RainBed {
    Sound* patter; // NULL without audio, or with no rain
    Sound* rumble;
    float open; // eased: 1 = in the rain, 0 = under cover
} RainBed;

// The two beds from `audio`, which may be NULL, playing silent until the first update.
void rain_bed_start(RainBed* bed, AudioSystem* audio, const Rain* rain);

// Per frame: ask the map about the listener's head, ease toward its answer, set the levels.
void rain_bed_update(RainBed* bed, const Rain* rain, ShadowSystem* shadows, const vec3 head,
                     float dt);

#endif // _SILENT_RAIN_BED_H_
