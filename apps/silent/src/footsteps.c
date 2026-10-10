#include <math.h>
#include <stdio.h>

#include "bedroom.h"
#include "footsteps.h"
#include "hill.h"
#include "home.h"
#include "interior.h"
#include "kitchen.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "mansion.h"
#include "woods.h"

// The files of each surface's takes: assets/audio/silent/step_<name>_<n>.wav, n from 1.
static const char* SURFACE_NAMES[FOOT_SURFACES] = {"wood",         "tile",   "cloth", "concrete",
                                                   "pavement_wet", "gravel", "grass", "grass_wet",
                                                   "forest",       "mud",    "water"};

// A walking step on boards, and every surface's as a share of it: cloth and grass are quiet
// under a shoe, boards, gravel and water are not.
#define STEP_VOLUME 0.3f
static const float SURFACE_VOLUME[FOOT_SURFACES] = {1.0f, 0.9f,  0.35f, 0.8f, 0.9f, 1.0f,
                                                    0.6f, 0.75f, 0.8f,  0.9f, 1.0f};

#define SETTLE_SHARE  0.6f // the foot set down beside the other as the body stops
#define SHUFFLE_SHARE 0.4f // a foot moved round turning on the spot
#define LAND_PER_FALL 0.2f // a landing, louder by this for each m/s it came down at
#define LAND_MOST     1.8f
#define WALK_SPEED    1.6f // the pace whose steps are STEP_VOLUME
#define JOG_FROM      2.2f // faster than this the feet come down harder
#define JOG_SHARE     1.35f
#define LOUDNESS_JIT  0.19f // +-1.5 dB, as a share
#define PITCH_JIT     0.04f
#define FOOT_APART    0.12f // each foot off the body's middle
#define SAY_GAP       0.5   // seconds between takes under --footstep-say

static uint32_t next_random(Footsteps* f) {
    f->rng ^= f->rng << 13;
    f->rng ^= f->rng >> 17;
    f->rng ^= f->rng << 5;
    return f->rng;
}

// Between -1 and 1.
static float jitter(Footsteps* f) {
    return 2.0f * (float)(next_random(f) >> 8) / 16777216.0f - 1.0f;
}

void footsteps_start(Footsteps* f, AudioSystem* audio, const Entity* body, uint32_t seed) {
    *f = (Footsteps){.audio = audio, .body = body, .rng = seed ? seed : 1u};
    for (int s = 0; s < FOOT_SURFACES; s++) {
        f->last[s] = -1;
        if (!audio)
            continue;
        // As many takes as there are files, from the first: a surface not fetched yet is silent
        // rather than an error at every load.
        for (int k = 0; k < FOOTSTEP_TAKES; k++) {
            char path[128];
            snprintf(path, sizeof(path), "assets/audio/silent/step_%s_%d.wav", SURFACE_NAMES[s],
                     k + 1);
            FILE* file = fopen(path, "rb");
            if (!file)
                break;
            fclose(file);
            Sound* take = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
            if (!take)
                break;
            f->takes[s][f->count[s]++] = take;
        }
    }
}

const char* footsteps_surface_name(FootSurface surface) {
    return surface < FOOT_SURFACES ? SURFACE_NAMES[surface] : "none";
}

// x and z inside a rectangle.
static bool within(float x, float z, float x0, float x1, float z0, float z1) {
    return x >= x0 && x <= x1 && z >= z0 && z <= z1;
}

// What the plan says the feet are on, dry.
static FootSurface ground(const vec3 feet) {
    const float x = feet[0], y = feet[1], z = feet[2];
    const vec3 plan = {x - MANSION_X, y - MANSION_Y, z - MANSION_Z};
    if (lake_on_dock(feet))
        return FOOT_WOOD;
    const float shore = lake_shore_distance(x, z);
    if (shore < 0.0f && y < LAKE_Y)
        return FOOT_WATER;
    if (shore < 1.5f && y < LAKE_Y + 0.4f)
        return FOOT_MUD;
    if (home_on_cloth(feet) || bedroom_on_cloth(feet) || kitchen_on_mat(feet) ||
        interior_on_cloth(plan) || mansion_on_cloth(plan))
        return FOOT_CLOTH;

    // The player's house: the kitchen's and the bathroom's tiles, boards elsewhere, and under the
    // floor the cellar flight's treads and the basement's slab.
    if (within(x, z, HOUSE_X0, HOUSE_X1, HOUSE_FRONT_Z, HOUSE_BACK_Z)) {
        if (y > FLOOR_Y - 0.15f) {
            if (within(x, z, HALL_X1, HOUSE_X1, HOUSE_FRONT_Z, KITCHEN_BACK_Z) ||
                within(x, z, HALL_X1, BATH_X1, KITCHEN_BACK_Z, HOME_SPLIT_Z))
                return FOOT_TILE;
            return FOOT_WOOD;
        }
        return y > BASEMENT_Y + 0.1f ? FOOT_WOOD : FOOT_CONCRETE;
    }
    if (within(x, z, PORCH_X0, PORCH_X1, PORCH_Z0, HOUSE_FRONT_Z) && y > FLOOR_Y - 0.1f)
        return FOOT_WOOD;
    // The mansion, its tower and its porch, on its floor or above: boards throughout.
    if (within(plan[0], plan[2], HOUSE_X0 - TOWER_OUTER - 0.5f, HOUSE_X1 + 0.5f, PORCH_Z0 - 0.5f,
               HOUSE_BACK_Z + 0.5f) &&
        plan[1] > FLOOR_Y - 0.1f)
        return FOOT_WOOD;
    // The cabin's floor and its porch, and the gravel pad it stands on.
    if (within(x, z, CABIN_PORCH_X0 - 0.4f, CABIN_X1, CABIN_Z0, CABIN_Z1) &&
        y > CABIN_PAD_Y + 0.15f)
        return FOOT_WOOD;
    if (within(x, z, CABIN_PAD_X0, CABIN_PAD_X1, CABIN_PAD_Z0, CABIN_PAD_Z1))
        return FOOT_GRAVEL;

    // The street and the cross street: asphalt and their sidewalks, all one to a shoe when dry.
    // Our path up to the porch.
    if (within(x, fabsf(z), CROSS_X0, STREET_HALF_LEN, 0.0f, STREET_HALF_WIDTH) ||
        within(fabsf(x - CROSS_X), z, 0.0f, STREET_HALF_WIDTH, CROSS_Z0, CROSS_Z1) ||
        within(x, z, PATH_X0, PATH_X1, STREET_HALF_WIDTH, PORCH_Z0))
        return FOOT_CONCRETE;
    if (hill_drive_distance(x, z) < DRIVE_HALF)
        return FOOT_CONCRETE;
    if (lake_track_distance(x, z) < TRACK_HALF)
        return FOOT_GRAVEL;

    // Off the ground: a neighbour's porch above it, and the terrace's flights below it, climbing
    // through notches cut in the lots.
    const float lift = y - land_height(x, z);
    if (lift > 0.15f)
        return FOOT_WOOD;
    if (lift < -0.15f)
        return FOOT_CONCRETE;
    return woods_cover(x, z) > 0.0f ? FOOT_FOREST : FOOT_GRASS;
}

FootSurface footsteps_surface(const vec3 feet, bool wet) {
    const FootSurface s = ground(feet);
    if (wet && s == FOOT_CONCRETE)
        return FOOT_PAVEMENT_WET;
    if (wet && s == FOOT_GRASS)
        return FOOT_GRASS_WET;
    return s;
}

// One of a surface's takes, never the one played last.
static int pick(Footsteps* f, FootSurface s) {
    const int n = f->count[s];
    if (n <= 1)
        return 0;
    int k = (int)(next_random(f) % (uint32_t)(n - 1));
    if (k >= f->last[s])
        k++;
    f->last[s] = k;
    return k;
}

static void play(Footsteps* f, FootSurface s, float volume, const vec3 at) {
    if (!f->audio || f->count[s] == 0)
        return;
    const int k = pick(f, s);
    AudioVoiceDesc d = {.follow = f->body,
                        .volume = volume * (1.0f + LOUDNESS_JIT * jitter(f)),
                        .pitch = 1.0f + PITCH_JIT * jitter(f)};
    glm_vec3_copy((float*)at, d.position);
    audio_play_voice(f->audio, f->takes[s][k], &d);
    if (f->trace)
        printf("footstep %s take %d volume %.3f pitch %.3f\n", SURFACE_NAMES[s], k + 1,
               (double)d.volume, (double)d.pitch);
}

void footsteps_fall(Footsteps* f, const GaitFootfall* fall, FootSurface surface, const vec3 feet,
                    const vec3 right) {
    float share = 1.0f;
    float side = fall->foot ? 1.0f : -1.0f;
    switch (fall->kind) {
        case GAIT_FALL_STEP:
            share = 0.75f + 0.25f * fminf(fall->speed / WALK_SPEED, 1.0f);
            if (fall->speed > JOG_FROM)
                share *= JOG_SHARE;
            break;
        case GAIT_FALL_SETTLE:
            share = SETTLE_SHARE;
            break;
        case GAIT_FALL_SHUFFLE:
            share = SHUFFLE_SHARE;
            break;
        case GAIT_FALL_LAND:
            share = fminf(1.0f + LAND_PER_FALL * fall->speed, LAND_MOST);
            side = 0.0f;
            break;
    }
    if (f->trace)
        printf("footstep fall on %s, %d takes\n", SURFACE_NAMES[surface], f->count[surface]);
    vec3 at;
    glm_vec3_copy((float*)feet, at);
    glm_vec3_muladds((float*)right, side * FOOT_APART, at);
    play(f, surface, STEP_VOLUME * SURFACE_VOLUME[surface] * share, at);
}

void footsteps_say(Footsteps* f, double time, const vec3 feet) {
    if ((double)f->said * SAY_GAP > time)
        return;
    int k = f->said++;
    for (int s = 0; s < FOOT_SURFACES; s++) {
        if (k < f->count[s]) {
            if (f->audio) {
                AudioVoiceDesc d = {.follow = f->body, .volume = STEP_VOLUME * SURFACE_VOLUME[s]};
                glm_vec3_copy((float*)feet, d.position);
                audio_play_voice(f->audio, f->takes[s][k], &d);
                printf("footstep say %s take %d\n", SURFACE_NAMES[s], k + 1);
            }
            return;
        }
        k -= f->count[s];
    }
}
