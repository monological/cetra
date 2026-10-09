#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/program.h"
#include "cetra/shader.h"
#include "cetra/shader_hook.h"

#include "home.h"
#include "mats.h"
#include "silent_shaders.h"
#include "sounds.h"
#include "tv.h"

// The set on its stand against the living room's back wall, facing the sofa, and its picture in
// its own frame: across it, up from the floor, and the glass's face, which the picture lies on.
static const KitFrame TV_SET = {{-TV_X, FLOOR_Y, LIVING_IN_Z1}, GLM_PIf};
#define PICTURE_A0 -0.27f
#define PICTURE_A1 0.17f
#define PICTURE_Y0 0.6f
#define PICTURE_Y1 0.96f
#define PICTURE_D  0.512f

// The signal: NTSC's 480 visible lines and 59.94 fields a second, and the ~440 values a line's
// 4.2 MHz of luma bandwidth carries across its active 52.6 us. The hum bar rolls once a beat of
// the mains against the field rate.
#define TV_LINES      480.0f
#define TV_SAMPLES    440.0f
#define TV_FIELD_HZ   59.94
#define TV_HUM_PERIOD (1.0 / (60.0 - TV_FIELD_HZ))
// The snow: the black level and the noise's spread at a gain of 1, which the clip and the tube's
// gamma turn into mostly dark with bright specks.
#define TV_BLACK 0.28f
#define TV_SIGMA 0.20f
#define TV_GAMMA 2.4f
// The tube's peak white, nits, at a colour set's cool white, about 9300 K, linear.
#define TV_PEAK_NITS 90.0f
static const vec3 TV_WHITE = {0.86f, 0.93f, 1.0f};
// The tube: its corners' radius and how soft its edge is, in picture heights; how far its light
// falls toward the sides; and how far the hum bar takes it down.
#define TV_CORNER    0.08f
#define TV_EDGE      0.01f
#define TV_FALLOFF   0.15f
#define TV_HUM_DEPTH 0.18f
// Showing nothing: a faint blue-grey on the glass, a set left on between programmes.
static const vec3 TV_IDLE = {0.55f, 0.66f, 0.82f};
#define TV_IDLE_NITS 4.0f
// The hiss, against the other loops tools/fetch_sounds.py levels alike: a set left on low.
#define TV_HISS_VOLUME 0.15f

// The AGC's gain for field `field`: a slow breath about the gain that fills the picture, which is
// what moves the light a screen of snow throws -- a field's specks cannot, ~200,000 of them.
static float tv_gain(double field) {
    const double t = field / TV_FIELD_HZ;
    return 1.0f + 0.05f * (float)sin(2.0 * GLM_PI * 0.53 * t) +
           0.03f * (float)sin(2.0 * GLM_PI * 1.7 * t + 1.0);
}

// The signal's mean light as a fraction of peak at a noise spread of `spread`: the expectation of
// the level clipped at black and white and through the gamma, a unit Gaussian summed at its
// midpoints across six sigma.
static float tv_mean(float spread) {
    const int steps = 480;
    double sum = 0.0, weights = 0.0;
    for (int i = 0; i < steps; i++) {
        const double g = -6.0 + (i + 0.5) * 12.0 / steps;
        const double w = exp(-0.5 * g * g);
        sum += w * pow(glm_clamp(TV_BLACK + spread * (float)g, 0.0f, 1.0f), TV_GAMMA);
        weights += w;
    }
    return (float)(sum / weights);
}

// The picture's light over its whole area as a share of the signal's mean: the hum bar's dip
// averaged over one cycle, which the picture's height is; the falloff averaged over the rectangle;
// and what the rounded corners and half of the soft edge round the outline leave of it.
static float tv_picture_average(float aspect) {
    const float outline = 2.0f * (aspect + 1.0f) - (8.0f - 2.0f * GLM_PIf) * TV_CORNER;
    const float cut = (4.0f - GLM_PIf) * TV_CORNER * TV_CORNER + 0.5f * TV_EDGE * outline;
    return (1.0f - 0.5f * TV_HUM_DEPTH) * (1.0f - 2.0f * TV_FALLOFF / 3.0f) * (1.0f - cut / aspect);
}

// `a` then `b` as one string; NULL with no memory.
static char* tv_join(const char* a, const char* b) {
    const size_t na = strlen(a), nb = strlen(b);
    char* out = malloc(na + nb + 1);
    if (out) {
        memcpy(out, a, na);
        memcpy(out + na, b, nb + 1);
    }
    return out;
}

// The picture: a card on the glass, the static's program over it in the late draw, and the
// glass's hook, which gives its light the picture's shape. Each from tv_picture.glsl and its own
// file, so the glass and the picture are one statement of the tube and the hum bar.
static void tv_picture(Tv* tv, Kit* kit, Engine* engine) {
    tv->picture = kit->materials[MAT_TV_STATIC];
    kit_frame_card_rect(kit, &TV_SET, MAT_TV_STATIC, (float[4]){0.0f, 0.0f, 1.0f, 1.0f}, PICTURE_A0,
                        PICTURE_A1, PICTURE_Y0, PICTURE_Y1, PICTURE_D, 1.0f);
    // In the late pass whether its program builds or not, so one that does not is refused by name
    // at the draw rather than drawn as a lit card over the glass.
    tv->picture->pass = MATERIAL_PASS_LATE_DRAW;
    char* snow_source =
        shader_source_splice(tv_static_shader_str, "// TV_PICTURE_CHUNK", tv_picture_shader_str);
    ShaderProgram* snow =
        snow_source ? create_late_surface_program("tv_static", snow_source) : NULL;
    free(snow_source);
    if (snow) {
        engine_add_program(engine, snow);
        material_set_program(tv->picture, snow);
    }

    const float width = PICTURE_A1 - PICTURE_A0, height = PICTURE_Y1 - PICTURE_Y0;
    tv->average = tv_picture_average(width / height);
    vec4 tube = {TV_CORNER, TV_EDGE, TV_FALLOFF, TV_HUM_DEPTH};
    ShaderParams* p = &tv->picture->shader_params;
    shader_params_set(p, "tvTube", tube);
    shader_params_set(p, "tvSignal", (vec4){TV_LINES, TV_SAMPLES, TV_BLACK, TV_GAMMA});
    shader_params_set(p, "tvWhite",
                      (vec4){TV_PEAK_NITS * TV_WHITE[0], TV_PEAK_NITS * TV_WHITE[1],
                             TV_PEAK_NITS * TV_WHITE[2], width / height});

    // The glass's light, placed in the picture by where it is: its UVs are the smear's.
    char* glass_source = tv_join(tv_picture_shader_str, tv_glass_shader_str);
    ShaderHook* hook = glass_source ? create_shader_hook(engine,
                                                         &(ShaderHookDesc){
                                                             .name = "tv_glass",
                                                             .surface = glass_source,
                                                         })
                                    : NULL;
    free(glass_source);
    if (!hook)
        fprintf(stderr, "silent: the television's glass glows flat, its hook unbuilt\n");
    tv->glass->shader_hook = hook;
    vec3 corner = {0.0f, 0.0f, 0.0f}, across = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&TV_SET, PICTURE_A0, PICTURE_Y0, PICTURE_D, corner);
    glm_vec3_add(corner, kit->origin, corner);
    kit_frame_dir(&TV_SET, 1.0f, 0.0f, 0.0f, across);
    ShaderParams* g = &tv->glass->shader_params;
    shader_params_set(g, "tvTube", tube);
    shader_params_set(g, "tvPlace", (vec4){corner[0], corner[1], corner[2], width});
    shader_params_set(g, "tvAcross", (vec4){across[0], across[1], across[2], height});
}

void tv_build(Tv* tv, Kit* kit, Engine* engine, Scene* scene, bool on) {
    *tv = (Tv){0};
    // The cabinet on its stand, the glass, two knobs and rabbit ears.
    const KitFrame* f = &TV_SET;
    kit_frame_box(kit, f, MAT_WOOD, -0.5f, 0.5f, 0.0f, 0.5f, 0.0f, 0.45f, true);
    kit_frame_box(kit, f, MAT_WOOD, -0.36f, 0.36f, 0.5f, 1.05f, 0.02f, 0.5f, true);
    kit_frame_box(kit, f, MAT_SCREEN, PICTURE_A0, PICTURE_A1, PICTURE_Y0, PICTURE_Y1, 0.5f,
                  PICTURE_D, false);
    for (int k = 0; k < 2; k++)
        kit_frame_lathe_on(kit, f, MAT_BLACK, (vec3){0.25f, 0.88f - 0.12f * (float)k, 0.5f},
                           (vec3){0.0f, 0.0f, 1.0f},
                           (vec2[]){{0.0f, 0.0f}, {0.018f, 0.0f}, {0.016f, 0.015f}, {0.0f, 0.017f}},
                           4, 8);
    for (int s = -1; s <= 1; s += 2) {
        const vec3 ear[] = {{0.0f, 1.05f, 0.25f}, {(float)s * 0.22f, 1.5f, 0.2f}};
        kit_frame_pipe(kit, f, MAT_STEEL, ear, 2, 0.003f, 5);
    }
    // The speaker, in the cabinet's face below the knobs.
    kit_frame_point(f, 0.25f, 0.66f, 0.5f, tv->speaker);
    glm_vec3_add(tv->speaker, kit->origin, tv->speaker);

    // The glass glows with what the set shows, as decoration: the set's light below is its light.
    tv->glass = kit->materials[MAT_SCREEN];
    tv->glass->emissive_light = 1;
    glm_vec3_copy((float*)(on ? TV_WHITE : TV_IDLE), tv->glass->emissive);
    tv->glass->emissive_strength = TV_IDLE_NITS;

    // The set's light on the room: a panel the picture's size lying on the glass and shining into
    // the room, as bright as the glass is on average. Shining only forward, from the glass, it
    // lights neither the glass nor the wall behind the set, and the fog scatters no panel, so it
    // hangs no glowing ball in the air as a point in front of the screen did.
    vec3 centre = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (PICTURE_A0 + PICTURE_A1), 0.5f * (PICTURE_Y0 + PICTURE_Y1),
                    PICTURE_D + 0.002f, centre);
    glm_vec3_add(centre, kit->origin, centre);
    kit_frame_dir(f, 0.0f, 0.0f, 1.0f, forward);
    LightDesc glow = {.name = "tv_glow",
                      .type = LIGHT_AREA,
                      .position = {centre[0], centre[1], centre[2]},
                      .direction = {forward[0], forward[1], forward[2]},
                      .up = {0.0f, 1.0f, 0.0f},
                      .size = {PICTURE_A1 - PICTURE_A0, PICTURE_Y1 - PICTURE_Y0},
                      .intensity = TV_IDLE_NITS,
                      .range = 4.0f};
    glm_vec3_copy((float*)(on ? TV_WHITE : TV_IDLE), glow.color);
    tv->glow = create_light(&glow);
    // The glass's own light is what reflects in SSR and the probes; a highlight of the panel as
    // well would be the screen reflected twice.
    tv->glow->specular = 0.0f;
    scene_add_light(scene, tv->glow);

    if (on) {
        tv_picture(tv, kit, engine);
        tv_update(tv, 0.0);
    }
}

void tv_start_audio(Tv* tv, AudioSystem* audio) {
    if (!tv->picture)
        return;
    Sound* hiss = sounds_loop(audio, "assets/audio/silent/tv_static.flac");
    if (hiss) {
        audio_sound_set_position(hiss, tv->speaker);
        audio_sound_set_volume(hiss, TV_HISS_VOLUME);
    }
}

// A field onto the picture, the glass and the light: its number, the hum's phase, the noise's
// spread and the signal's mean.
static void tv_show(Tv* tv, const vec4 field) {
    shader_params_set(&tv->picture->shader_params, "tvField", field);
    shader_params_set(&tv->glass->shader_params, "tvField", field);
    // The glass's light is the signal's mean, which its hook shapes; a glass whose hook did not
    // build glows with it flat.
    tv->glass->emissive_strength = TV_PEAK_NITS * field[3];
    tv->glow->intensity = TV_PEAK_NITS * field[3] * tv->average;
}

void tv_update(Tv* tv, double time) {
    if (!tv->picture)
        return;
    const double field = floor(time * TV_FIELD_HZ);
    const float spread = TV_SIGMA * tv_gain(field);
    // The field's number wraps where a float stops counting exactly, an even count so its parity
    // holds; the glass and the picture show the same field.
    glm_vec4_copy((vec4){(float)fmod(field, 16777216.0), (float)fmod(time / TV_HUM_PERIOD, 1.0),
                         spread, tv_mean(spread)},
                  tv->field);
    tv_show(tv, tv->field);
}

void tv_rest(Tv* tv, bool rest) {
    if (!tv->picture)
        return;
    // At rest: the first field, the hum at its start and the gain at its mean, which is 1.
    if (rest)
        tv_show(tv, (vec4){0.0f, 0.0f, TV_SIGMA, tv_mean(TV_SIGMA)});
    else
        tv_show(tv, tv->field);
}
