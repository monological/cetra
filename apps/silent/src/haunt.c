#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cetra/program.h"
#include "cetra/shader_params.h"

#include "basement.h"
#include "door_takes.h"
#include "haunt.h"
#include "kit.h"
#include "layout.h"
#include "mats.h"
#include "silent_shaders.h"

_Static_assert(HAUNT_KNOB_TAKES == DOOR_KNOB_TAKES, "a held sound for every knob take");

// As it begins the night outside goes and the hall's light dies with it; the hall comes back red
// this many seconds later, at HALL_GAIN times what it gave: red is the dimmest third of white to
// the eye, and a hall at its own candela reads as dark rather than red. The glass keeps its own
// nits, or it burns through the tone curve to pink.
#define HALL_DARK_SECONDS 0.6
static const vec3 HALL_RED = {1.0f, 0.02f, 0.01f};
#define HALL_GAIN 2.5f
// A failing supply never quite goes out: these lights cast cached shadows, and a cached light at
// nothing leaves its shadow undrawn (basement.c's bulb, spec 13.31).
#define SUPPLY_FLOOR 0.03f
// How often each circuit has an episode: the hall's often enough to keep the eye on it, the lamp's
// seldom enough to catch it out.
#define HALL_PERIOD 5.0
#define LAMP_PERIOD 7.0
// The set's white as the hall goes red: its red lifted so the snow is as bright to the eye as a
// red can be made, its others all but gone.
static const vec3 TV_RED = {1.8f, 0.04f, 0.03f};
// The picture, once the hall is red: drained to this red over GRADE_SECONDS, its edges taken
// further down, and a little grain over it.
static const vec3 GRADE_RED = {1.5f, 0.1f, 0.08f};
#define GRADE_AMOUNT          0.8f
#define GRADE_SECONDS         2.5
#define GRADE_VIGNETTE        0.85f
#define GRADE_VIGNETTE_RADIUS 0.4f
#define GRADE_GRAIN           0.04f

// The drone the house goes quiet under, risen over DRONE_RISE_SECONDS: two low tones a beat apart
// that throb against each other, the tritone between them and their octave -- what a laptop's
// speaker, which gives nothing under about 100 Hz, carries of it. Volumes against a tone's own
// amplitude, a quarter of full scale: under the house's own sounds, or it covers what comes up the
// stair.
static const struct {
    float hz;
    float volume;
} DRONE[HAUNT_DRONE_TONES] = {{55.0f, 0.05f}, {55.6f, 0.05f}, {77.8f, 0.015f}, {110.9f, 0.025f}};
#define DRONE_RISE_SECONDS 6.0

// Something comes up the cellar flight from its foot, from CLIMB_AT: a footfall every CLIMB_STEP
// seconds, the last three slower, heavy and a little low. Then the bathroom door drifts a hand's
// width open with a creak, and then the cellar door is tried. The volumes are against the one-shots
// tools/fetch_sounds.py levels alike, and are loud on purpose: a sound falls as one over its
// distance, and the climb is heard through the cellar door besides.
#define CLIMB_AT      3.0
#define CLIMB_STEP    0.95
#define CLIMB_SLOW    1.2
#define CLIMB_VOLUME  5.0f
#define CLIMB_PITCH   0.88f
#define CREAK_AT      16.5
#define CREAK_ANGLE   0.125f // radians: a hand's width at a house door's latch
#define CREAK_SECONDS 2.5f
#define CREAK_VOLUME  1.5f
#define RATTLE_AT     19.0
#define SLAM_VOLUME   2.0f
// A try at the cellar door: its knob turned and pulled, and from the third try on the door shaken
// in its frame RATTLE_LAG in, the leaf pulled up to RATTLE_PLAY metres off its stop at the latch
// edge on each of the take's hits; the knob's own hits move it KNOB_SHAKE as far. The tries come
// harder over RATTLE_GROW seconds, with a pause of TRY_GAP to TRY_GAP + TRY_SPREAD between them.
#define RATTLE_LAG    0.3
#define RATTLE_PLAY   0.012f
#define RATTLE_GROW   20.0
#define KNOB_SHAKE    0.35f
#define TRY_GAP       0.8
#define TRY_SPREAD    1.7
#define KNOB_VOLUME   3.5f
#define RATTLE_VOLUME 3.5f
// The quiet: a player within this far of the cellar door in plan, on its floor, stops everything
// at the door. Farther than the action key reaches, so it always comes first.
#define QUIET_REACH 2.2f
// The door beats wait to be watched for: the player in the front of the hall, short of WATCH_Z1
// under the pendant, looking down it within the angle whose cosine is WATCH_COS, from WATCH_AFTER
// seconds in, once the hall has come back red.
#define WATCH_Z1    13.2f
#define WATCH_COS   0.85f
#define WATCH_AFTER 1.5

// 1 inside a..b, coming up and going down over `edge` seconds: a filament's warmth, which keeps a
// light from snapping on and off like a strobe.
static float pulse(float t, float a, float b, float edge) {
    return glm_smoothstep(a - edge, a, t) * (1.0f - glm_smoothstep(b, b + edge, t));
}

/*
 * A failing circuit at `u` seconds in: steady, and once a `period` an episode, its moment and its
 * kind picked by the period's hash -- a blink or two, a sag that comes back slowly as a filament
 * heats again, or a fall out that catches once and is gone again before it comes back. An episode
 * runs under two seconds and starts where it still ends within its period.
 */
static float failing(double u, double period, unsigned int seed) {
    const uint32_t cycle = (uint32_t)floor(u / period);
    const float t = (float)(u - (double)cycle * period);
    const float at = 0.5f + ((float)period - 2.7f) * kit_hash01(cycle, 30u, seed);
    const float kind = kit_hash01(cycle, 31u, seed);
    const float a = kit_hash01(cycle, 32u, seed), b = kit_hash01(cycle, 33u, seed);
    float low = SUPPLY_FLOOR, dip = 0.0f;
    if (kind < 0.45f) {
        const float off = 0.07f + 0.08f * a;
        dip = pulse(t, at, at + off, 0.03f);
        if (b < 0.5f) {
            const float again = at + off + 0.12f + 0.2f * b;
            dip = fmaxf(dip, pulse(t, again, again + off, 0.03f));
        }
    } else if (kind < 0.8f) {
        const float hold = 0.4f + 0.8f * b;
        low = 0.3f + 0.3f * a;
        dip = glm_smoothstep(at, at + 0.15f, t) *
              (1.0f - glm_smoothstep(at + hold, at + hold + 0.5f, t));
    } else {
        const float out = 0.6f + 0.6f * a;
        dip = pulse(t, at, at + out + 0.3f, 0.04f) - pulse(t, at + out, at + out + 0.06f, 0.02f);
    }
    return 1.0f - (1.0f - low) * glm_clamp_zo(dip);
}

// The house's lights as `s` has them.
static void apply(Haunt* h, const HauntLive* s) {
    const HauntNormal* n = &h->normal;
    const float gain = 1.0f + (HALL_GAIN - 1.0f) * s->red;
    for (int i = 0; i < 2; i++) {
        Light* l = h->hall[i];
        if (!l)
            continue;
        glm_vec3_lerp((float*)n->hall_colour[i], (float*)HALL_RED, s->red, l->color);
        l->intensity = n->hall_candela[i] * gain * s->hall;
    }
    Material* glass = h->house.frosted;
    if (glass) {
        glm_vec3_lerp((float*)n->glass, (float*)HALL_RED, s->red, glass->emissive);
        glass->emissive_strength = n->glass_nits * s->hall;
    }
    if (h->lamp) {
        h->lamp->intensity = n->lamp_candela * s->lamp;
        mats_shade_candela(h->house.shade, h->shade_slot, n->lamp_candela * s->lamp);
    }
    // The night outside gone: a pane passes on what is behind it tinted by its own colour, so a
    // black one shows nothing and keeps only its shine. The street lamps cast no shadow, so they
    // light the house through its walls, and they go out with it.
    const float out = 1.0f - s->dark_out;
    if (h->house.panes)
        glm_vec3_scale((float*)n->panes, out, h->house.panes->albedo);
    if (h->house.lamp_heads)
        h->house.lamp_heads->emissive_strength = n->lamp_head_nits * out;
    for (int i = 0; i < h->street_count; i++)
        h->street[i]->intensity = n->street_candela[i] * out;
}

// The set's white `red` of the way to red. Not part of apply: the set rests itself (tv_rest).
static void tint_tv(Haunt* h, float red) {
    if (!h->house.tv)
        return;
    vec3 tint = {1.0f, 1.0f, 1.0f};
    glm_vec3_lerp(tint, (float*)TV_RED, red, tint);
    tv_tint(h->house.tv, tint);
}

// The picture `w` of the way to the haunting's, its finishing put back exactly at none. Not part
// of apply either: no capture sees the post chain.
static void grade(Haunt* h, float w) {
    PostFX* fx = h->house.engine ? h->house.engine->postfx : NULL;
    if (!fx)
        return;
    const HauntNormal* n = &h->normal;
    if (h->grade) {
        h->grade->enabled = w > 0.0f;
        shader_params_set(&h->grade->params, "hauntGrade",
                          (vec4){GRADE_RED[0], GRADE_RED[1], GRADE_RED[2], GRADE_AMOUNT * w});
    }
    if (w <= 0.0f) {
        fx->vignette_enabled = n->vignette;
        fx->vignette_strength = n->vignette_strength;
        fx->vignette_radius = n->vignette_radius;
        fx->grain_enabled = n->grain;
        fx->grain_strength = n->grain_strength;
        return;
    }
    fx->vignette_enabled = true;
    fx->vignette_strength = glm_lerp(n->vignette_strength, GRADE_VIGNETTE, w);
    fx->vignette_radius = glm_lerp(n->vignette_radius, GRADE_VIGNETTE_RADIUS, w);
    fx->grain_enabled = true;
    fx->grain_strength = glm_lerp(n->grain ? n->grain_strength : 0.0f, GRADE_GRAIN, w);
}

// When footfall `k` of the climb lands, in seconds since it began.
static double climb_at(int k) {
    const int fast = BASEMENT_CLIMB - 3;
    return CLIMB_AT + CLIMB_STEP * (double)(k < fast ? k : fast) +
           CLIMB_SLOW * (double)(k > fast ? k - fast : 0);
}

// A try at the cellar door: when it starts, seconds into the rattle; which knob take it turns; and
// whether the door is shaken with it, and when it is all over.
typedef struct Try {
    double at;
    int knob;
    bool shake;
    double end;
} Try;

// The try under way at `u` seconds into the rattle, or the last one begun, walked from the first:
// the first two only the knob, as though whoever is there tried it before pulling.
static Try rattle_try(double u, unsigned int seed) {
    double at = 0.0;
    for (uint32_t k = 0;; k++) {
        Try t = {.at = at,
                 .knob = (int)(kit_hash01(k, 40u, seed) * (float)DOOR_KNOB_TAKES) % DOOR_KNOB_TAKES,
                 .shake = k >= 2 && kit_hash01(k, 41u, seed) < 0.8f};
        double len = DOOR_KNOB[t.knob].seconds;
        if (t.shake)
            len = fmax(len, RATTLE_LAG + DOOR_RATTLE[0].seconds);
        t.end = at + len;
        const double next = t.end + TRY_GAP + TRY_SPREAD * kit_hash01(k, 42u, seed);
        if (u < next)
            return t;
        at = next;
    }
}

// The leaf off its stop around a hit `d` seconds from it: pulled out over the 50 ms before, the
// latch hitting the strike, and let fall back over the 90 ms after.
static float hit_pull(double d) {
    return d < 0.0 ? glm_smoothstep(-0.05f, 0.0f, (float)d)
                   : 1.0f - glm_smoothstep(0.0f, 0.09f, (float)d);
}

// The largest pull of `take`'s hits, started at `at`, at `u`, each as strong as its hit.
static float take_pull(const DoorTake* take, double at, double u) {
    float pull = 0.0f;
    for (int i = 0; i < take->hits; i++)
        pull = fmaxf(pull, take->strength[i] * hit_pull(u - (at + (double)take->at[i])));
    return pull;
}

// How hard the tries come at `u` seconds into the rattle: harder over its first RATTLE_GROW.
static float rattle_force(double u) {
    return 0.7f + 0.3f * glm_smoothstep(0.0f, 1.0f, (float)(u / RATTLE_GROW));
}

// The cellar door's leaf `u` seconds into the rattle: radians off its stop.
static float rattle_jolt(const Haunt* h, double u) {
    const Try t = rattle_try(u, h->seed);
    float pull = KNOB_SHAKE * take_pull(&DOOR_KNOB[t.knob], t.at, u);
    if (t.shake)
        pull = fmaxf(pull, take_pull(&DOOR_RATTLE[0], t.at + RATTLE_LAG, u));
    const Door* door = h->house.cellar;
    const float width = door->shape.to - door->shape.from;
    return RATTLE_PLAY / width * rattle_force(u) * pull;
}

// A fire-and-forget copy of `s` at `at`, world.
static void voice(const Haunt* h, const Sound* s, const vec3 at, float volume, float pitch) {
    if (!s || !h->house.audio)
        return;
    AudioVoiceDesc d = {.volume = volume, .pitch = pitch};
    glm_vec3_copy((float*)at, d.position);
    audio_play_voice(h->house.audio, s, &d);
}

// The held takes at the cellar door, stopped.
static void hush_door(Haunt* h) {
    for (int i = 0; i < HAUNT_KNOB_TAKES; i++)
        if (h->knob[i])
            audio_sound_stop(h->knob[i]);
    if (h->rattle)
        audio_sound_stop(h->rattle);
    if (h->house.cellar)
        door_jolt(h->house.cellar, 0.0f);
}

// Whether a player at `feet` looking along `forward` watches for the door beats: in the front of
// the hall, between the front door and the pendant, looking down it toward the bathroom and
// cellar doors.
static bool watching(const vec3 feet, const vec3 forward) {
    const bool front = feet[0] > HALL_X0 && feet[0] < HALL_X1 && feet[2] > HOUSE_FRONT_Z &&
                       feet[2] < WATCH_Z1 && feet[1] > FLOOR_Y - 0.3f;
    const float across = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    return front && across > 1e-4f && forward[2] > WATCH_COS * across;
}

// The house as it was before it began.
static const HauntLive NORMAL = {1.0f, 1.0f, 0.0f, 0.0f};

void haunt_start(Haunt* h, const HauntHouse* house, unsigned int seed) {
    *h = (Haunt){.house = *house, .seed = seed, .shade_slot = -1};
    Scene* scene = house->scene;
    if (!scene)
        return;
    h->hall[0] = scene_find_light(scene, "hall_bulb");
    h->hall[1] = scene_find_light(scene, "hall_lantern");
    h->lamp = scene_find_light(scene, "floor_lamp");
    if (h->lamp)
        h->shade_slot = mats_shade_slot(house->shade, h->lamp->original_position);
    if (!h->hall[0] || !h->hall[1] || !h->lamp || h->shade_slot < 0)
        fprintf(stderr, "silent: the haunting finds%s%s%s%s\n", h->hall[0] ? "" : " no hall bulb",
                h->hall[1] ? "" : " no hall lantern", h->lamp ? "" : " no floor lamp",
                h->shade_slot >= 0 ? "" : " no lampshade bulb");
    for (size_t i = 0; i < scene->light_count && h->street_count < HAUNT_STREET_LAMPS; i++) {
        Light* l = scene->lights[i];
        if (l->name && !strcmp(l->name, "street_lamp"))
            h->street[h->street_count++] = l;
    }
    Engine* engine = house->engine;
    ShaderProgram* program = engine && engine->postfx
                                 ? create_post_pass_program("haunt_grade", haunt_grade_shader_str)
                                 : NULL;
    if (program) {
        engine_add_program(engine, program);
        h->grade = postfx_add_pass(engine->postfx, POSTFX_AT_AFTER_TONEMAP, program);
        if (h->grade)
            h->grade->enabled = false;
    } else if (engine && engine->postfx) {
        fprintf(stderr, "silent: the haunting's picture is not drained to red, its pass unbuilt\n");
    }
    AudioSystem* audio = house->audio;
    if (!audio)
        return;
    // The drone is heard whole wherever the player stands: it is in their head, not in a room.
    for (int i = 0; i < HAUNT_DRONE_TONES; i++) {
        h->drone[i] = audio_sound_from_tone(audio, DRONE[i].hz, AUDIO_BUS_SFX);
        if (h->drone[i])
            audio_sound_set_looping(h->drone[i], true);
    }
    // The climb takes the player's own steps on boards, its own copies so a run without footsteps
    // still hears it.
    char path[96];
    for (int i = 0; i < HAUNT_STEP_TAKES; i++) {
        snprintf(path, sizeof(path), "assets/audio/silent/step_wood_%d.wav", i + 1);
        h->steps[i] = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
    }
    h->creak = audio_sound_from_file(audio, "assets/audio/silent/door_creak.wav", AUDIO_BUS_SFX);
    h->slam = audio_sound_from_file(audio, "assets/audio/silent/door_slam.wav", AUDIO_BUS_SFX);
    // The tries are heard from the leaf's hall side, where the knob and the latch are: in the hall,
    // not through the door.
    for (int i = 0; i <= HAUNT_KNOB_TAKES; i++) {
        const DoorTake* take = i < HAUNT_KNOB_TAKES ? &DOOR_KNOB[i] : &DOOR_RATTLE[0];
        snprintf(path, sizeof(path), "assets/audio/silent/%s", take->file);
        Sound* s = audio_sound_from_file(audio, path, AUDIO_BUS_SFX);
        if (s && house->cellar && house->cellar->entity)
            audio_sound_set_position(s, house->cellar->entity->position);
        if (i < HAUNT_KNOB_TAKES)
            h->knob[i] = s;
        else
            h->rattle = s;
    }
}

// The drone `rise` of the way up, from silent.
static void drone_up(Haunt* h, float rise) {
    for (int i = 0; i < HAUNT_DRONE_TONES; i++)
        if (h->drone[i])
            audio_sound_set_volume(h->drone[i], DRONE[i].volume * rise);
}

void haunt_begin(Haunt* h, double time, double into) {
    if (h->active)
        return;
    // What the house stands at now, which is what it goes back to.
    HauntNormal* n = &h->normal;
    for (int i = 0; i < 2; i++)
        if (h->hall[i]) {
            glm_vec3_copy(h->hall[i]->color, n->hall_colour[i]);
            n->hall_candela[i] = h->hall[i]->intensity;
        }
    if (h->house.frosted) {
        glm_vec3_copy(h->house.frosted->emissive, n->glass);
        n->glass_nits = h->house.frosted->emissive_strength;
    }
    if (h->lamp)
        n->lamp_candela = h->lamp->intensity;
    if (h->house.panes)
        glm_vec3_copy(h->house.panes->albedo, n->panes);
    if (h->house.lamp_heads)
        n->lamp_head_nits = h->house.lamp_heads->emissive_strength;
    for (int i = 0; i < h->street_count; i++)
        n->street_candela[i] = h->street[i]->intensity;
    const PostFX* fx = h->house.engine ? h->house.engine->postfx : NULL;
    if (fx) {
        n->vignette = fx->vignette_enabled;
        n->vignette_strength = fx->vignette_strength;
        n->vignette_radius = fx->vignette_radius;
        n->grain = fx->grain_enabled;
        n->grain_strength = fx->grain_strength;
    }
    h->active = true;
    h->began = time - into;
    h->live = NORMAL;
    h->quiet = false;
    h->slamming = false;
    // The door beats wait to be watched for -- unless this begins part-way in, when they are as far
    // in, and the doors stand where they would by then: the cellar door shut, and past the creak
    // the bathroom door ajar.
    h->doors_at = -1.0;
    Door* cellar = h->house.cellar;
    if (into > 0.0 && cellar) {
        h->doors_at = h->began;
        h->last_u = into;
        if (cellar->travel > 0.0f || cellar->want > 0.0f)
            door_set(cellar, 0.0f, 0.0f);
        Door* bath = h->house.bath;
        if (bath && into >= CREAK_AT && bath->travel <= 0.0f && bath->want <= 0.0f) {
            const float ajar = door_travel_at(bath, CREAK_ANGLE);
            door_set(bath, ajar, ajar);
        }
    }
    // The house goes quiet as its lights die, as a supply failing would have it: the clock stops
    // dead, the fridge's compressor cuts out with a clunk, and the drone starts up from nothing.
    if (h->house.clock)
        clock_stop(h->house.clock, true);
    if (h->house.fridge)
        kitchen_fridge_run(h->house.fridge, false);
    drone_up(h, 0.0f);
    for (int i = 0; i < HAUNT_DRONE_TONES; i++)
        if (h->drone[i])
            audio_sound_play(h->drone[i]);
    if (h->trace)
        printf("haunt begin at %.3f, %.3f s in\n", time, into);
}

void haunt_end(Haunt* h) {
    if (!h->active)
        return;
    apply(h, &NORMAL);
    tint_tv(h, 0.0f);
    grade(h, 0.0f);
    for (int i = 0; i < HAUNT_DRONE_TONES; i++)
        if (h->drone[i])
            audio_sound_stop(h->drone[i]);
    // A door slamming goes on shutting, unheard; the bathroom door stands where it drifted to.
    hush_door(h);
    h->slamming = false;
    if (h->house.clock)
        clock_stop(h->house.clock, false);
    if (h->house.fridge)
        kitchen_fridge_run(h->house.fridge, true);
    h->active = false;
    if (h->trace)
        printf("haunt end\n");
}

bool haunt_active(const Haunt* h) {
    return h->active;
}

void haunt_step(Haunt* h, double time, const vec3 feet, const vec3 forward) {
    Door* cellar = h->house.cellar;
    if (!h->active || !cellar)
        return;
    if (h->slamming && !cellar->slam) {
        h->slamming = false;
        voice(h, h->slam, cellar->entity->position, SLAM_VOLUME, 1.0f);
        if (h->trace)
            printf("haunt slam at %.3f\n", time - h->began);
    }
    if (h->quiet)
        return;
    if (h->doors_at < 0.0) {
        if (time - h->began < WATCH_AFTER || !watching(feet, forward))
            return;
        // Watched for: the door beats begin, and a cellar door standing open slams shut in front of
        // the player.
        h->doors_at = time;
        h->last_u = 0.0;
        if (cellar->travel > 0.0f || cellar->want > 0.0f) {
            door_slam(cellar);
            h->slamming = true;
        }
        if (h->trace)
            printf("haunt watched at %.3f\n", time - h->began);
        return;
    }
    const double u = time - h->doors_at, was = h->last_u;
    h->last_u = u;

    // The quiet: once something is on the stair, a player coming near the door stops it all dead
    // -- the climb, the tries, the drone -- and nothing at the door comes again.
    const float* door_at = cellar->entity->position;
    const float dx = feet[0] - door_at[0], dz = feet[2] - door_at[2];
    if (u >= CLIMB_AT && feet[1] > FLOOR_Y - 0.3f &&
        dx * dx + dz * dz < QUIET_REACH * QUIET_REACH) {
        h->quiet = true;
        hush_door(h);
        drone_up(h, 0.0f);
        if (h->trace)
            printf("haunt quiet at %.3f, %.2f m from the door\n", u, sqrtf(dx * dx + dz * dz));
        return;
    }

    for (int k = 0; k < BASEMENT_CLIMB; k++) {
        const double at = climb_at(k);
        if (was >= at || at > u)
            continue;
        vec3 foot = {0.0f, 0.0f, 0.0f};
        basement_footfall(k, (k & 1) ? 0.1f : -0.1f, foot);
        const int take = (int)(kit_hash01((uint32_t)k, 50u, h->seed) * (float)HAUNT_STEP_TAKES);
        const float pitch = CLIMB_PITCH + 0.08f * (kit_hash01((uint32_t)k, 51u, h->seed) - 0.5f);
        voice(h, h->steps[take % HAUNT_STEP_TAKES], foot, CLIMB_VOLUME, pitch);
        if (h->trace)
            printf("haunt footfall %d at %.3f\n", k, u);
    }

    Door* bath = h->house.bath;
    if (bath && was < CREAK_AT && CREAK_AT <= u && bath->travel <= 0.0f && bath->want <= 0.0f) {
        door_drift(bath, door_travel_at(bath, CREAK_ANGLE), CREAK_SECONDS);
        voice(h, h->creak, bath->entity->position, CREAK_VOLUME, 1.0f);
        if (h->trace)
            printf("haunt creak at %.3f\n", u);
    }

    // The tries, only at a shut door: one the player has opened is not tried.
    if (u < RATTLE_AT || cellar->travel > 0.0f || cellar->want > 0.0f) {
        door_jolt(cellar, 0.0f);
        return;
    }
    const double r = u - RATTLE_AT, r_was = was - RATTLE_AT;
    const Try t = rattle_try(r, h->seed);
    const float force = rattle_force(r);
    Sound* knob = h->knob[t.knob];
    if (r_was < t.at && t.at <= r && knob) {
        audio_sound_set_volume(knob, KNOB_VOLUME * force);
        audio_sound_play(knob);
        if (h->trace)
            printf("haunt try at %.3f, knob %d%s\n", u, t.knob + 1, t.shake ? ", shaken" : "");
    }
    if (t.shake && r_was < t.at + RATTLE_LAG && t.at + RATTLE_LAG <= r && h->rattle) {
        audio_sound_set_volume(h->rattle, RATTLE_VOLUME * force);
        audio_sound_play(h->rattle);
    }
    door_jolt(cellar, rattle_jolt(h, r));
}

void haunt_frame(Haunt* h, double time) {
    if (!h->active)
        return;
    const double u = time - h->began;
    const bool dark = u < HALL_DARK_SECONDS;
    h->live = (HauntLive){
        .hall = dark ? SUPPLY_FLOOR : failing(u, HALL_PERIOD, h->seed),
        .lamp = failing(u, LAMP_PERIOD, h->seed ^ 0x9e3779b9u),
        .red = dark ? 0.0f : 1.0f,
        .dark_out = 1.0f,
    };
    apply(h, &h->live);
    tint_tv(h, h->live.red);
    grade(h, glm_smoothstep(0.0f, 1.0f, (float)((u - HALL_DARK_SECONDS) / GRADE_SECONDS)));
    drone_up(h, h->quiet ? 0.0f : glm_smoothstep(0.0f, 1.0f, (float)(u / DRONE_RISE_SECONDS)));
}

void haunt_rest(Haunt* h, bool rest) {
    if (h->active)
        apply(h, rest ? &NORMAL : &h->live);
}
