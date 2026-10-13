#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cetra/gi_volume.h"
#include "cetra/ibl.h"
#include "cetra/probe_set.h"
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

// As it begins every light it drives goes out with the night outside -- the hall's, the lamp's and
// the set's -- and the picture is drained to red unseen in the dark. BLACKOUT_SECONDS later they
// come back up over RISE_SECONDS, already red, the hall at HALL_GAIN times what it gave: red is
// the dimmest third of white to the eye, and with the bounce off every wall gone with the power,
// the hall is lit by its two lights alone. The glass keeps its own nits, or it burns through the
// tone curve to pink.
#define BLACKOUT_SECONDS 1.5
#define RISE_SECONDS     1.2
static const vec3 HALL_RED = {1.0f, 0.02f, 0.01f};
#define HALL_GAIN 6.0f
// Out is SUPPLY_OUT of the supply and a failing one never falls under SUPPLY_FLOOR, never nothing:
// these lights cast cached shadows, and a cached light at nothing leaves its shadow undrawn
// (basement.c's bulb, spec 13.31).
#define SUPPLY_OUT   1e-4f
#define SUPPLY_FLOOR 0.03f
// How often each circuit has an episode: the hall's often enough to keep the eye on it, the lamp's
// seldom enough to catch it out.
#define HALL_PERIOD 5.0
#define LAMP_PERIOD 7.0
// The set's white as the hall goes red: its red lifted so the snow is as bright to the eye as a
// red can be made, its others all but gone.
static const vec3 TV_RED = {1.8f, 0.04f, 0.03f};
// The picture while it is haunted: drained to this red, its edges taken further down, and a little
// grain over it.
static const vec3 GRADE_RED = {1.5f, 0.1f, 0.08f};
#define GRADE_AMOUNT          0.8f
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

// The bathroom door creaks at CREAK_AT: a shut one drifts a hand's width open over CREAK_SECONDS,
// an open one drifts shut over CREAK_SHUT_SECONDS, the creak's own length. Then from RATTLE_AT the
// cellar door is tried. The volumes are against the one-shots tools/fetch_sounds.py levels alike,
// and are loud on purpose: a sound falls as one over its distance.
#define CREAK_AT           3.0
#define CREAK_ANGLE        0.125f // radians: a hand's width at a house door's latch
#define CREAK_SECONDS      2.5f
#define CREAK_SHUT_SECONDS 3.0f
#define CREAK_VOLUME       1.5f
#define RATTLE_AT          6.0
#define SLAM_VOLUME        2.0f
// A try at the cellar door: its knob turned and pulled TRY_TURNS_MIN to TRY_TURNS_MAX times over,
// a take after another, and from the second try on the door shaken in its frame all through it
// from RATTLE_LAG in, the rattle's take after another, the leaf pulled up to RATTLE_PLAY metres
// off its stop at the latch edge on each of the take's hits; the knob's own hits move it
// KNOB_SHAKE as far. The tries come harder over RATTLE_GROW seconds, from RATTLE_FORCE0 to
// RATTLE_FORCE1 of their volume and their pull, with a pause of TRY_GAP to TRY_GAP + TRY_SPREAD
// between them.
#define TRY_TURNS_MIN 2
#define TRY_TURNS_MAX 4
#define RATTLE_LAG    0.3
#define RATTLE_PLAY   0.03f
#define RATTLE_GROW   20.0
#define RATTLE_FORCE0 0.8f
#define RATTLE_FORCE1 1.2f
#define KNOB_SHAKE    0.5f
#define TRY_GAP       0.4
#define TRY_SPREAD    1.0
#define KNOB_VOLUME   4.0f
#define RATTLE_VOLUME 5.0f
// The quiet: once the door beats have begun, the player opening the cellar door stops everything
// at it dead, and it flies open and slams against its stop; the stair's bulb comes on red, at
// CELLAR_GAIN times its own light, swinging CELLAR_SWING times as hard as a draft swings it.
#define CELLAR_GAIN  4.0f
#define CELLAR_SWING 3.0f
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
    // Every other light in the house on the share of its supply the cut leaves it.
    if (h->bath)
        h->bath->intensity = n->bath_candela * s->mains;
    if (h->house.bulbs)
        h->house.bulbs->emissive_strength = n->bulb_nits * s->mains;
    if (h->bedside) {
        h->bedside->intensity = n->bedside_candela * s->mains;
        mats_shade_candela(h->house.shade, h->bedside_slot, n->bedside_candela * s->mains);
    }
    // The light the house was captured in -- its bounce off every wall and its reflections -- is
    // the light of lamps the cut has put out, so it is not seen; the captures, which keep it, see
    // it. Nor is the sky's, which with no bounce to say how much of it a room sees is reflected
    // whole in every glossy surface indoors.
    Scene* scene = h->house.scene;
    if (scene && scene->gi)
        scene->gi->hidden = !s->ambient;
    ReflectionProbeSet* set = scene ? scene->probe_set : NULL;
    for (int i = 0; set && i < h->probe_count; i++)
        set->probes[i]->intensity = s->ambient ? n->probe_intensity[i] : 0.0f;
    if (scene && scene->ibl)
        scene->ibl->intensity = s->ambient ? n->sky : 0.0f;
}

// The lights the house's own modules drive, the kitchen's tubes and the cellar's bulb, on `share`
// of their supply from now, in their own colour. Not part of apply: each rests itself.
static void house_supply(Haunt* h, float share) {
    if (h->house.lights)
        lights_set_mains(h->house.lights, share);
    Basement* b = h->house.basement;
    if (b) {
        b->mains = share;
        glm_vec3_one(b->tint);
        b->gain = 1.0f;
    }
}

// The set's white `red` of the way to red, at `level` of its own brightness. Not part of apply: the
// set rests itself (tv_rest).
static void tint_tv(Haunt* h, float red, float level) {
    if (!h->house.tv)
        return;
    vec3 tint = {1.0f, 1.0f, 1.0f};
    glm_vec3_lerp(tint, (float*)TV_RED, red, tint);
    glm_vec3_scale(tint, level, tint);
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
        glm_vec3_copy((float*)n->fog_ambient, fx->fog_ambient);
        fx->fog_ambient_from_sky = n->fog_from_sky;
        return;
    }
    fx->vignette_enabled = true;
    fx->vignette_strength = glm_lerp(n->vignette_strength, GRADE_VIGNETTE, w);
    fx->vignette_radius = glm_lerp(n->vignette_radius, GRADE_VIGNETTE_RADIUS, w);
    fx->grain_enabled = true;
    fx->grain_strength = glm_lerp(n->grain ? n->grain_strength : 0.0f, GRADE_GRAIN, w);
    // The fog glows by a light no wall stops, which fills the rooms as a haze and the windows with
    // the street's fog: put out with the rest, leaving it lit only by the lamps that are on.
    fx->fog_ambient_from_sky = false;
    glm_vec3_scale((float*)n->fog_ambient, 1.0f - w, fx->fog_ambient);
}

// A try at the cellar door: when it starts, seconds into the rattle; the knob takes it turns, one
// after another, and when each starts from the try's start; how many of the rattle's takes it
// shakes the door through, one after another from RATTLE_LAG in, 0 for the knob alone; and when it
// is all over.
typedef struct Try {
    double at;
    int turns;
    int knob[TRY_TURNS_MAX];
    double turn_at[TRY_TURNS_MAX];
    int shakes;
    double end;
} Try;

// When shake `j` of a try starts, from the try's start.
static double shake_at(int j) {
    return RATTLE_LAG + (double)j * (double)DOOR_RATTLE[0].seconds;
}

// The try under way at `u` seconds into the rattle, or the last one begun, walked from the first:
// the first only the knob, as though whoever is there tried it before pulling. No take turns twice
// running, its held sound being one.
static Try rattle_try(double u, unsigned int seed) {
    double at = 0.0;
    for (uint32_t k = 0;; k++) {
        Try t = {.at = at};
        const int span = TRY_TURNS_MAX - TRY_TURNS_MIN + 1;
        t.turns = TRY_TURNS_MIN + (int)(kit_hash01(k, 43u, seed) * (float)span) % span;
        double len = 0.0;
        for (int i = 0; i < t.turns; i++) {
            const float pick = kit_hash01(k, 40u + 16u * (uint32_t)i, seed);
            t.knob[i] = i == 0 ? (int)(pick * DOOR_KNOB_TAKES) % DOOR_KNOB_TAKES
                               : (t.knob[i - 1] + 1 +
                                  (int)(pick * (DOOR_KNOB_TAKES - 1)) % (DOOR_KNOB_TAKES - 1)) %
                                     DOOR_KNOB_TAKES;
            t.turn_at[i] = len;
            len += DOOR_KNOB[t.knob[i]].seconds;
        }
        if (k >= 1) {
            t.shakes = (int)ceil((len - RATTLE_LAG) / (double)DOOR_RATTLE[0].seconds);
            t.shakes = t.shakes < 1 ? 1 : t.shakes;
            len = fmax(len, shake_at(t.shakes));
        }
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
    return glm_lerp(RATTLE_FORCE0, RATTLE_FORCE1,
                    glm_smoothstep(0.0f, 1.0f, (float)(u / RATTLE_GROW)));
}

// The cellar door's leaf `u` seconds into the rattle: radians off its stop.
static float rattle_jolt(const Haunt* h, double u) {
    const Try t = rattle_try(u, h->seed);
    float pull = 0.0f;
    for (int i = 0; i < t.turns; i++)
        pull = fmaxf(pull, KNOB_SHAKE * take_pull(&DOOR_KNOB[t.knob[i]], t.at + t.turn_at[i], u));
    for (int j = 0; j < t.shakes; j++)
        pull = fmaxf(pull, take_pull(&DOOR_RATTLE[0], t.at + shake_at(j), u));
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
static const HauntLive NORMAL = {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, true};

void haunt_start(Haunt* h, const HauntHouse* house, unsigned int seed) {
    *h = (Haunt){.house = *house, .seed = seed, .shade_slot = -1, .bedside_slot = -1};
    Scene* scene = house->scene;
    if (!scene)
        return;
    h->hall[0] = scene_find_light(scene, "hall_bulb");
    h->hall[1] = scene_find_light(scene, "hall_lantern");
    h->lamp = scene_find_light(scene, "floor_lamp");
    if (h->lamp)
        h->shade_slot = mats_shade_slot(house->shade, h->lamp->original_position);
    h->bath = scene_find_light(scene, "bath_bulb");
    h->bedside = scene_find_light(scene, "bedside_lamp");
    if (h->bedside)
        h->bedside_slot = mats_shade_slot(house->shade, h->bedside->original_position);
    if (!h->hall[0] || !h->hall[1] || !h->lamp || h->shade_slot < 0 || !h->bath || !h->bedside ||
        h->bedside_slot < 0)
        fprintf(stderr, "silent: the haunting finds%s%s%s%s%s%s%s\n",
                h->hall[0] ? "" : " no hall bulb", h->hall[1] ? "" : " no hall lantern",
                h->lamp ? "" : " no floor lamp", h->shade_slot >= 0 ? "" : " no lampshade bulb",
                h->bath ? "" : " no bathroom bulb", h->bedside ? "" : " no bedside lamp",
                h->bedside_slot >= 0 ? "" : " no bedside lampshade bulb");
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
    char path[96];
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
    if (h->bath)
        n->bath_candela = h->bath->intensity;
    if (h->bedside)
        n->bedside_candela = h->bedside->intensity;
    if (h->house.bulbs)
        n->bulb_nits = h->house.bulbs->emissive_strength;
    if (h->house.panes)
        glm_vec3_copy(h->house.panes->albedo, n->panes);
    if (h->house.lamp_heads)
        n->lamp_head_nits = h->house.lamp_heads->emissive_strength;
    for (int i = 0; i < h->street_count; i++)
        n->street_candela[i] = h->street[i]->intensity;
    const ReflectionProbeSet* set = h->house.scene ? h->house.scene->probe_set : NULL;
    const size_t probes = set ? set->residency.count : 0;
    h->probe_count = probes < HAUNT_PROBES ? (int)probes : HAUNT_PROBES;
    for (int i = 0; i < h->probe_count; i++)
        n->probe_intensity[i] = set->probes[i]->intensity;
    if (h->house.scene && h->house.scene->ibl)
        n->sky = h->house.scene->ibl->intensity;
    const PostFX* fx = h->house.engine ? h->house.engine->postfx : NULL;
    if (fx) {
        n->vignette = fx->vignette_enabled;
        n->vignette_strength = fx->vignette_strength;
        n->vignette_radius = fx->vignette_radius;
        n->grain = fx->grain_enabled;
        n->grain_strength = fx->grain_strength;
        glm_vec3_copy((float*)fx->fog_ambient, n->fog_ambient);
        n->fog_from_sky = fx->fog_ambient_from_sky;
    }
    h->active = true;
    h->began = time - into;
    h->live = NORMAL;
    h->quiet = false;
    h->slamming = false;
    // The door beats wait to be watched for -- unless this begins part-way in, when they are as far
    // in, and the doors stand where they would by then: the cellar door shut, and past the creak
    // the bathroom door ajar if it was shut and shut if it was open.
    h->watched = false;
    Door* cellar = h->house.cellar;
    if (into > 0.0 && cellar) {
        h->watched = true;
        h->doors_at = h->began;
        h->last_u = into;
        if (cellar->travel > 0.0f || cellar->want > 0.0f)
            door_set(cellar, 0.0f, 0.0f);
        Door* bath = h->house.bath;
        if (bath && into >= CREAK_AT) {
            const bool shut = bath->travel <= 0.0f && bath->want <= 0.0f;
            const float to = shut ? door_travel_at(bath, CREAK_ANGLE) : 0.0f;
            door_set(bath, to, to);
        }
    }
    // The house goes quiet as its lights die, as a supply failing would have it: the clock stops
    // dead, the fridge's compressor cuts out with a clunk, the tubes and the cellar's bulb go out
    // for as long as it lasts, and the drone starts up from nothing.
    if (h->house.clock)
        clock_stop(h->house.clock, true);
    if (h->house.fridge)
        kitchen_fridge_run(h->house.fridge, false);
    house_supply(h, SUPPLY_OUT);
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
    house_supply(h, 1.0f);
    tint_tv(h, 0.0f, 1.0f);
    if (h->house.tv)
        tv_hiss(h->house.tv, 1.0f);
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
    if (!h->watched) {
        if (time - h->began < WATCH_AFTER || !watching(feet, forward))
            return;
        // Watched for: the door beats begin, and a cellar door standing open slams shut in front of
        // the player.
        h->watched = true;
        h->doors_at = time;
        h->last_u = 0.0;
        if (cellar->travel > 0.0f || cellar->want > 0.0f) {
            door_slam(cellar, 0.0f);
            h->slamming = true;
        }
        if (h->trace)
            printf("haunt watched at %.3f\n", time - h->began);
        return;
    }
    const double u = time - h->doors_at, was = h->last_u;
    h->last_u = u;

    // The quiet: the player opening the cellar door -- the doors have just moved, so its want is
    // the action key's -- stops it all dead, the tries and the drone, and the door is flung open
    // and slams against its stop, on the stair's bulb lit red and swinging hard. Nothing at the
    // door comes again.
    if (cellar->want > 0.0f && !cellar->slam && !h->slamming) {
        h->quiet = true;
        hush_door(h);
        drone_up(h, 0.0f);
        door_slam(cellar, 1.0f);
        h->slamming = true;
        Basement* b = h->house.basement;
        if (b) {
            b->mains = 1.0f;
            glm_vec3_copy((float*)HALL_RED, b->tint);
            b->gain = CELLAR_GAIN;
            basement_swing(b, time, CELLAR_SWING);
        }
        if (h->trace)
            printf("haunt flung open at %.3f\n", u);
        return;
    }

    Door* bath = h->house.bath;
    if (bath && was < CREAK_AT && CREAK_AT <= u) {
        const bool shut = bath->travel <= 0.0f && bath->want <= 0.0f;
        if (shut)
            door_drift(bath, door_travel_at(bath, CREAK_ANGLE), CREAK_SECONDS);
        else
            door_drift(bath, 0.0f, CREAK_SHUT_SECONDS);
        voice(h, h->creak, bath->entity->position, CREAK_VOLUME, 1.0f);
        if (h->trace)
            printf("haunt creak %s at %.3f\n", shut ? "open" : "shut", u);
    }

    // The tries, only at a shut door: one the player has opened is not tried.
    if (u < RATTLE_AT || cellar->travel > 0.0f || cellar->want > 0.0f) {
        door_jolt(cellar, 0.0f);
        return;
    }
    const double r = u - RATTLE_AT, r_was = was - RATTLE_AT;
    const Try t = rattle_try(r, h->seed);
    const float force = rattle_force(r);
    if (h->trace && r_was < t.at && t.at <= r)
        printf("haunt try at %.3f, %d turns, %d shakes\n", u, t.turns, t.shakes);
    for (int i = 0; i < t.turns; i++) {
        const double at = t.at + t.turn_at[i];
        Sound* knob = h->knob[t.knob[i]];
        if (r_was < at && at <= r && knob) {
            audio_sound_set_volume(knob, KNOB_VOLUME * force);
            audio_sound_play(knob);
        }
    }
    for (int j = 0; j < t.shakes; j++) {
        const double at = t.at + shake_at(j);
        if (r_was < at && at <= r && h->rattle) {
            audio_sound_set_volume(h->rattle, RATTLE_VOLUME * force);
            audio_sound_play(h->rattle);
        }
    }
    door_jolt(cellar, rattle_jolt(h, r));
}

void haunt_frame(Haunt* h, double time) {
    if (!h->active)
        return;
    const double u = time - h->began;
    // Out, then up to each circuit's own failing, its episodes counted from when it came back.
    const double back = fmax(0.0, u - BLACKOUT_SECONDS);
    const float up = glm_smoothstep(0.0f, 1.0f, (float)(back / RISE_SECONDS));
    const float on = SUPPLY_OUT + (1.0f - SUPPLY_OUT) * up;
    h->live = (HauntLive){
        .hall = on * failing(back, HALL_PERIOD, h->seed),
        .lamp = on * failing(back, LAMP_PERIOD, h->seed ^ 0x9e3779b9u),
        .red = 1.0f,
        .dark_out = 1.0f,
        .mains = SUPPLY_OUT,
        .ambient = false,
    };
    apply(h, &h->live);
    tint_tv(h, 1.0f, on);
    if (h->house.tv)
        tv_hiss(h->house.tv, up);
    grade(h, 1.0f);
    drone_up(h, h->quiet ? 0.0f : glm_smoothstep(0.0f, 1.0f, (float)(u / DRONE_RISE_SECONDS)));
}

void haunt_rest(Haunt* h, bool rest) {
    if (h->active)
        apply(h, rest ? &NORMAL : &h->live);
}
