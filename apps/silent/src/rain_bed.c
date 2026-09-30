#include "rain_bed.h"

#include <math.h>

// The loudest the rain gets, on the SFX bus, at the violent band's floor: a bed under the rest
// of the scene rather than over it.
#define RAIN_BED_GAIN        0.5f
#define RAIN_BED_LOUDEST_MMH 50.0f
// The rumble's share of the level, in the rain and under cover alike.
#define RAIN_BED_RUMBLE 0.45f
// What a roof leaves of the patter: the drops on the roof itself, not on the ground round you.
#define RAIN_BED_COVERED_PATTER 0.12f
// Seconds to ease between in the rain and under it: a doorway's worth of walking.
#define RAIN_BED_EASE_S 0.6f

// Loudness in proportion to the LOG of the rate, the way a sense of loudness goes: a drizzle is
// audible at all, and a downpour is not ten times a moderate rain.
static float rain_bed_level(const Rain* rain) {
    if (!rain || !(rain->rate_mmh > 0.0f))
        return 0.0f;
    float l = log1pf(rain->rate_mmh) / log1pf(RAIN_BED_LOUDEST_MMH);
    return RAIN_BED_GAIN * fminf(l, 1.0f);
}

void rain_bed_start(RainBed* bed, AudioSystem* audio, const Rain* rain) {
    bed->patter = bed->rumble = NULL;
    bed->open = 1.0f;
    if (!audio || !rain || !(rain->rate_mmh > 0.0f))
        return;
    bed->patter = audio_sound_from_noise(audio, AUDIO_NOISE_PINK, AUDIO_BUS_SFX);
    bed->rumble = audio_sound_from_noise(audio, AUDIO_NOISE_BROWN, AUDIO_BUS_SFX);
    Sound* beds[] = {bed->patter, bed->rumble};
    for (int i = 0; i < 2; i++) {
        audio_sound_set_volume(beds[i], 0.0f);
        audio_sound_play(beds[i]);
    }
}

void rain_bed_update(RainBed* bed, const Rain* rain, ShadowSystem* shadows, const vec3 head,
                     float dt) {
    if (!bed->patter && !bed->rumble)
        return;
    shadow_rain_cover_ask(shadows, head);
    float open = 1.0f;
    if (!shadow_rain_cover_answer(shadows, &open))
        open = bed->open; // no answer yet: hold what was heard
    bed->open += (open - bed->open) * (1.0f - expf(-fmaxf(dt, 0.0f) / RAIN_BED_EASE_S));
    const float level = rain_bed_level(rain);
    audio_sound_set_volume(bed->patter, level * (RAIN_BED_COVERED_PATTER +
                                                 (1.0f - RAIN_BED_COVERED_PATTER) * bed->open));
    audio_sound_set_volume(bed->rumble, level * RAIN_BED_RUMBLE);
}
