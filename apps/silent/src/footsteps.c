#include <math.h>
#include <stdio.h>

#include "cetra/util.h"

#include "bedroom.h"
#include "footsteps.h"
#include "hill.h"
#include "home.h"
#include "kitchen.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "mansion.h"
#include "mats.h"

// Each surface's takes, assets/audio/silent/step_<name>_<n>.wav with n from 1, and how loud a
// walking step on it is as a share of one on boards: cloth and grass are quiet under a shoe,
// boards, gravel and water are not.
static const struct {
    const char* name;
    float volume;
} SURFACES[FOOT_SURFACES] = {
    [FOOT_WOOD] = {"wood", 1.0f},
    [FOOT_TILE] = {"tile", 0.9f},
    [FOOT_CLOTH] = {"cloth", 0.35f},
    [FOOT_CONCRETE] = {"concrete", 0.8f},
    [FOOT_PAVEMENT_WET] = {"pavement_wet", 0.9f},
    [FOOT_GRAVEL] = {"gravel", 1.0f},
    [FOOT_GRASS] = {"grass", 0.6f},
    [FOOT_GRASS_WET] = {"grass_wet", 0.75f},
    [FOOT_FOREST] = {"forest", 0.8f},
    [FOOT_MUD] = {"mud", 0.9f},
    [FOOT_WATER] = {"water", 1.0f},
};

#define STEP_VOLUME   0.3f // a walking step on boards
#define SETTLE_SHARE  0.6f // the foot set down beside the other as the body stops
#define SHUFFLE_SHARE 0.4f // a foot moved round turning on the spot
#define LAND_PER_FALL 0.2f // a landing, louder by this for each m/s it came down at
#define LAND_MOST     1.8f
#define STROLL_SPEED  1.6f // the pace whose steps are STEP_VOLUME, m/s; slower is softer
#define JOG_FROM      2.2f // faster than this the feet come down harder
#define JOG_SHARE     1.35f
#define LOUDNESS_JIT  0.19f // +-1.5 dB, as a share
#define PITCH_JIT     0.04f
#define FOOT_APART    0.12f // each foot off the body's middle
#define WET_FROM      0.3f  // the rain's film on the ground past which a step on it sounds wet
#define PROBE_UP      0.3f  // the ray down starts this far over the feet
#define PROBE_DOWN    0.6f  // and looks this far under them
#define SAY_GAP       0.5   // seconds between takes in footsteps_say

// Between -1 and 1.
static float jitter(Footsteps* f) {
    return 2.0f * kit_xrnd(&f->rng) - 1.0f;
}

void footsteps_start(Footsteps* f, AudioSystem* audio, PhysicsWorld* physics,
                     const KitSurfaces* surfaces, uint32_t seed) {
    *f = (Footsteps){
        .audio = audio, .physics = physics, .surfaces = surfaces, .rng = seed ? seed : 1u};
    for (int s = 0; s < FOOT_SURFACES; s++) {
        f->last[s] = -1;
        if (!audio)
            continue;
        // As many takes as there are files, from the first: a surface not fetched yet is said once
        // and is silent, rather than an error at every load.
        for (int k = 0; k < FOOTSTEP_TAKES; k++) {
            char path[128];
            snprintf(path, sizeof(path), "assets/audio/silent/step_%s_%d.wav", SURFACES[s].name,
                     k + 1);
            if (!path_exists(path))
                break;
            Sound* take = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
            if (!take)
                break;
            f->takes[s][f->count[s]++] = take;
        }
        if (f->count[s] == 0)
            printf("silent: no footsteps on %s: assets/audio/silent/step_%s_1.wav is missing\n",
                   SURFACES[s].name, SURFACES[s].name);
    }
}

// What the kit's materials sound like underfoot; anything else hard is concrete.
static FootSurface mat_surface(int mat) {
    switch (mat) {
        case MAT_WOOD_FLOOR:
        case MAT_HARDWOOD:
        case MAT_PORCH:
        case MAT_WOOD:
        case MAT_MAHOGANY:
        case MAT_FENCE_BOARD:
        case MAT_LOG:
            return FOOT_WOOD;
        case MAT_KITCHEN_FLOOR:
        case MAT_BACKSPLASH:
        case MAT_CERAMIC:
            return FOOT_TILE;
        case MAT_RUG:
        case MAT_TOWEL:
        case MAT_CUSHION:
        case MAT_UPHOLSTERY:
        case MAT_BEDDING:
        case MAT_QUILT:
            return FOOT_CLOTH;
        case MAT_CELLAR_WET:
            return FOOT_PAVEMENT_WET;
        case MAT_TRACK:
            return FOOT_GRAVEL;
        case MAT_DIRT:
            return FOOT_GRASS;
        case MAT_WOODS_FLOOR:
            return FOOT_FOREST;
        case MAT_SHORE:
            return FOOT_MUD;
        default:
            return FOOT_CONCRETE;
    }
}

// What feet at `feet` stand on, dry.
static FootSurface ground(const Footsteps* f, const vec3 feet) {
    const float x = feet[0], z = feet[2];
    if (home_on_cloth(feet) || bedroom_on_cloth(feet) || kitchen_on_mat(feet) ||
        mansion_cloth_at(feet))
        return FOOT_CLOTH;
    if (feet[1] < LAKE_Y && lake_shore_distance(x, z) < 0.0f)
        return FOOT_WATER;
    RaycastHit hit = {0};
    vec3 from = {x, feet[1] + PROBE_UP, z};
    if (physics_world_raycast_filtered(f->physics, from, (vec3){0.0f, -1.0f, 0.0f},
                                       PROBE_UP + PROBE_DOWN, 1u << OBJ_LAYER_STATIC, &hit)) {
        const int mat = kit_surface_of(f->surfaces, hit.entity);
        if (mat != KIT_COLLIDER_ONLY)
            return mat_surface(mat);
    }
    // The open ground: a ribbon drawn over it, or its own facet.
    if (hill_drive_distance(x, z) < DRIVE_HALF)
        return FOOT_CONCRETE;
    if (lake_track_distance(x, z) < TRACK_HALF)
        return FOOT_GRAVEL;
    return mat_surface(land_mat_at(x, z));
}

// One of a surface's takes, never the one played last.
static int pick(Footsteps* f, FootSurface s) {
    const int n = f->count[s];
    if (n <= 1)
        return 0;
    const int last = f->last[s];
    // Any of the n before the first, any of the other n - 1 after it.
    const int choices = last < 0 ? n : n - 1;
    int k = (int)fminf(kit_xrnd(&f->rng) * (float)choices, (float)(choices - 1));
    if (last >= 0 && k >= last)
        k++;
    f->last[s] = k;
    return k;
}

static void play(Footsteps* f, FootSurface s, float volume, const vec3 at) {
    if (f->count[s] == 0) {
        if (f->trace)
            printf("footstep %s, no takes\n", SURFACES[s].name);
        return;
    }
    const int k = pick(f, s);
    AudioVoiceDesc d = {.volume = volume * (1.0f + LOUDNESS_JIT * jitter(f)),
                        .pitch = 1.0f + PITCH_JIT * jitter(f)};
    glm_vec3_copy((float*)at, d.position);
    audio_play_voice(f->audio, f->takes[s][k], &d);
    if (f->trace)
        printf("footstep %s take %d volume %.3f pitch %.3f\n", SURFACES[s].name, k + 1,
               (double)d.volume, (double)d.pitch);
}

void footsteps_fall(Footsteps* f, const GaitFootfall* fall, const vec3 feet, float yaw,
                    float wetness) {
    float share = 1.0f;
    float side = fall->foot ? 1.0f : -1.0f;
    switch (fall->kind) {
        case GAIT_FALL_NONE:
        case GAIT_FALL_KINDS:
            return;
        case GAIT_FALL_STEP:
            share = 0.75f + 0.25f * fminf(fall->speed / STROLL_SPEED, 1.0f);
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
    FootSurface s = ground(f, feet);
    if (wetness > WET_FROM && s == FOOT_CONCRETE)
        s = FOOT_PAVEMENT_WET;
    if (wetness > WET_FROM && s == FOOT_GRASS)
        s = FOOT_GRASS_WET;
    // The foot that fell stands off the body's middle along its right.
    const vec3 at = {feet[0] - cosf(yaw) * side * FOOT_APART, feet[1],
                     feet[2] + sinf(yaw) * side * FOOT_APART};
    play(f, s, STEP_VOLUME * SURFACES[s].volume * share, at);
}

void footsteps_say(Footsteps* f, double time, const vec3 feet) {
    if ((double)f->said * SAY_GAP > time)
        return;
    int k = f->said++;
    for (int s = 0; s < FOOT_SURFACES; s++) {
        if (k < f->count[s]) {
            AudioVoiceDesc d = {.volume = STEP_VOLUME * SURFACES[s].volume};
            glm_vec3_copy((float*)feet, d.position);
            audio_play_voice(f->audio, f->takes[s][k], &d);
            printf("footstep say %s take %d\n", SURFACES[s].name, k + 1);
            return;
        }
        k -= f->count[s];
    }
}
