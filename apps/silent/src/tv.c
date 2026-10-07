#include <math.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/program.h"

#include "home.h"
#include "mats.h"
#include "sounds.h"
#include "tv.h"

// The signal (spec 13.30): NTSC's 480 visible lines and 59.94 fields a second, and the ~440 values
// a line's 4.2 MHz of luma bandwidth carries across its active 52.6 us.
#define TV_LINES    480.0f
#define TV_SAMPLES  440.0f
#define TV_FIELD_HZ 59.94f
// The tube: its peak white in nits, and the corners' radius as a fraction of the picture's height.
#define TV_PEAK_NITS 90.0f
#define TV_CORNER    0.08f
// The snow: the black level and the noise's spread at a gain of 1, which the clip and the gamma
// turn into mostly dark with bright specks; and the hum bar, mains against the field rate, which
// rolls once every 1 / (60 - 59.94) seconds.
#define TV_BLACK      0.28f
#define TV_SIGMA      0.20f
#define TV_HUM_DEPTH  0.08f
#define TV_HUM_PERIOD 16.7f
// A colour set's cool white, about 9300 K, linear.
static const float TV_TINT[3] = {0.86f, 0.93f, 1.0f};
// The hiss, against the other loops tools/fetch_sounds.py levels alike: a set left on low.
#define TV_HISS_VOLUME 0.15f

// The AGC's gain, held for each field: a slow breath about the gain that fills the picture, which
// is what moves the light a screen of snow throws. The specks cannot: a field averages ~200,000.
static float tv_gain(double time) {
    const double t = floor(time * TV_FIELD_HZ) / TV_FIELD_HZ;
    return 1.0f + 0.05f * (float)sin(2.0 * GLM_PI * 0.53 * t) +
           0.03f * (float)sin(2.0 * GLM_PI * 1.7 * t + 1.0);
}

// The picture's mean light over its peak at gain `gain`: the expectation of the clipped level
// through the gamma, a Gaussian summed at its midpoints across six sigma, under the hum bar's
// average.
static float tv_mean(float gain) {
    const int steps = 480;
    double sum = 0.0, weights = 0.0;
    for (int i = 0; i < steps; i++) {
        const double g = -6.0 + (i + 0.5) * 12.0 / steps;
        const double w = exp(-0.5 * g * g);
        const double v = glm_clamp(TV_BLACK + TV_SIGMA * gain * (float)g, 0.0f, 1.0f);
        sum += w * pow(v, 2.4);
        weights += w;
    }
    return (float)(sum / weights) * (1.0f - 0.5f * TV_HUM_DEPTH);
}

static Light* tv_find_light(Scene* scene, const char* name) {
    for (size_t i = 0; i < scene->light_count; i++) {
        Light* light = scene->lights[i];
        if (light && light->name && strcmp(light->name, name) == 0)
            return light;
    }
    return NULL;
}

void tv_init(Tv* tv, const Kit* kit, Scene* scene, bool on) {
    *tv = (Tv){0};
    if (!on)
        return;
    tv->glass = kit->materials[MAT_SCREEN];
    tv->picture = kit->materials[MAT_TV_STATIC];
    tv->glow = tv_find_light(scene, "tv_glow");
    tv->glow_base = tv->glow ? tv->glow->intensity : 0.0f;
    tv->mean_base = tv_mean(1.0f);
    if (tv->glow)
        glm_vec3_copy((float*)TV_TINT, tv->glow->color);
    glm_vec3_copy((float*)TV_TINT, tv->glass->emissive);

    const float aspect = (TV_PICTURE_A1 - TV_PICTURE_A0) / (TV_PICTURE_Y1 - TV_PICTURE_Y0);
    ShaderParams* p = &tv->picture->shader_params;
    shader_params_set(p, "tvSignal", (vec4){TV_LINES, TV_SAMPLES, TV_FIELD_HZ, 0.0f});
    shader_params_set(p, "tvSnow", (vec4){TV_BLACK, TV_SIGMA, TV_HUM_DEPTH, TV_HUM_PERIOD});
    shader_params_set(p, "tvTint", (vec4){TV_TINT[0], TV_TINT[1], TV_TINT[2], aspect});
    tv_update(tv, 0.0, 1.0f);
}

void tv_start_audio(Tv* tv, AudioSystem* audio) {
    if (!tv->picture)
        return;
    tv->hiss = sounds_loop(audio, "assets/audio/silent/tv_static.flac");
    if (tv->hiss) {
        vec3 speaker = {0.0f, 0.0f, 0.0f};
        home_tv_speaker(speaker);
        audio_sound_set_position(tv->hiss, speaker);
    }
}

void tv_update(Tv* tv, double time, float hearing) {
    if (!tv->picture)
        return;
    const float gain = tv_gain(time);
    const float mean = tv_mean(gain);
    // The glass emits the mean, and the picture adds what the static is beyond it: their sum is
    // the static, whatever this mean misses of it, and what reflects the screen sees the mean.
    tv->glass->emissive_strength = TV_PEAK_NITS * mean;
    shader_params_set(&tv->picture->shader_params, "tvTube",
                      (vec4){TV_PEAK_NITS, gain, TV_PEAK_NITS * mean, TV_CORNER});
    if (tv->glow)
        tv->glow->intensity = tv->glow_base * mean / tv->mean_base;
    if (tv->hiss)
        audio_sound_set_volume(tv->hiss, TV_HISS_VOLUME * hearing);
}
