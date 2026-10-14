#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cetra/ibl.h"
#include "cetra/program.h"
#include "cetra/shader_params.h"

#include "basement.h"
#include "haunt.h"
#include "kit.h"
#include "layout.h"
#include "mats.h"
#include "silent_shaders.h"

// As it begins every light it drives goes out with the night outside -- the hall's, the lamp's and
// the set's -- and the picture is drained to red unseen in the dark. BLACKOUT_SECONDS later they
// come back up over RISE_SECONDS, already red, the hall at HALL_GAIN times what it gave: red is
// the dimmest third of white to the eye. The lanterns' filaments keep their own nits, or they burn
// through the tone curve to white.
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
// What it begins with, heard whole wherever the player is.
#define CUE_VOLUME 0.5f

// The bathroom door creaks CREAK_AT after the player first stands in the hall: a shut one drifts a
// hand's width open over CREAK_SECONDS, an open one drifts shut over CREAK_SHUT_SECONDS, the
// creak's own length. The cellar door is tried from RATTLE_AT after the haunting began, once it
// stands shut, until it is opened on the tries. The volumes are against the one-shots
// tools/fetch_sounds.py levels alike, and are loud on purpose: a sound falls as one over its
// distance.
#define CREAK_AT           1.0
#define CREAK_ANGLE        0.125f // radians: a hand's width at a house door's latch
#define CREAK_SECONDS      2.5f
#define CREAK_SHUT_SECONDS 3.0f
#define CREAK_VOLUME       1.5f
#define RATTLE_AT          4.0
// A try at the cellar door: its knob turned and pulled TRY_TURNS_MIN to TRY_TURNS_MAX times over,
// a take after another, and from the second try on the door shaken in its frame all through it,
// the rattle's take after another, the leaf pulled up to RATTLE_PLAY metres off its stop at the
// latch edge on each of the take's hits. The tries come harder over RATTLE_GROW seconds, from
// RATTLE_FORCE0 to RATTLE_FORCE1 of their volume and their pull, one straight after another.
#define TRY_TURNS_MIN 2
#define TRY_TURNS_MAX DOOR_TRY_TURNS
#define RATTLE_PLAY   0.03f
#define RATTLE_GROW   20.0
#define RATTLE_FORCE0 0.8f
#define RATTLE_FORCE1 1.2f
#define KNOB_VOLUME   4.0f
#define RATTLE_VOLUME 5.0f
// The hall's pendant swings this far, radians, once it begins, pushed by nothing anyone can see,
// easing into it over PENDANT_EASE seconds; when it ends the pendant is caught, still in
// PENDANT_CAUGHT.
#define PENDANT_SWING  0.18f
#define PENDANT_EASE   2.5f
#define PENDANT_CAUGHT 0.4f
// The slam waits to be watched for: the player in the front of the hall, short of WATCH_Z1 under
// the pendant, looking down it within the angle whose cosine is WATCH_COS, from WATCH_AFTER seconds
// in, once the hall has come back red.
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
    Material* filament = h->house.filament;
    if (filament) {
        glm_vec3_lerp((float*)n->filament, (float*)HALL_RED, s->red, filament->emissive);
        filament->emissive_strength = n->filament_nits * s->hall;
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
    // The light from no lamp -- the bounce the house was captured in, its reflections, and the
    // sky's -- is one dial, the environment's strength, which the lit surface multiplies all of it
    // by. The captures, which keep the bounce, see it whole.
    Scene* scene = h->house.scene;
    if (scene && scene->ibl)
        scene->ibl->intensity = n->sky * s->ambient;
}

// The lights the house's own modules drive, the kitchen's tubes and the cellar's bulb, on `share`
// of their supply from now. Not part of apply: each rests itself.
static void house_supply(Haunt* h, float share) {
    if (h->house.lights)
        lights_set_mains(h->house.lights, share);
    if (h->house.basement)
        h->house.basement->mains = share;
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

// The try under way at `u` seconds into the rattle, or the last one begun, walked from the first:
// the first only the knob, as though whoever is there tried it before pulling. No take turns twice
// running, its held sound being one.
static DoorTry rattle_try(double u, unsigned int seed) {
    double at = 0.0;
    for (uint32_t k = 0;; k++) {
        DoorTry t = {.at = at};
        const int span = TRY_TURNS_MAX - TRY_TURNS_MIN + 1;
        t.turns = TRY_TURNS_MIN + (int)(kit_hash01(k, 43u, seed) * (float)span) % span;
        for (int i = 0; i < t.turns; i++) {
            const float pick = kit_hash01(k, 40u + 16u * (uint32_t)i, seed);
            t.knob[i] = i == 0 ? (int)(pick * DOOR_KNOB_TAKES) % DOOR_KNOB_TAKES
                               : (t.knob[i - 1] + 1 +
                                  (int)(pick * (DOOR_KNOB_TAKES - 1)) % (DOOR_KNOB_TAKES - 1)) %
                                     DOOR_KNOB_TAKES;
        }
        door_try_lay(&t, k >= 1);
        if (u < t.end)
            return t;
        at = t.end;
    }
}

// How hard the tries come at `u` seconds into the rattle: harder over its first RATTLE_GROW.
static float rattle_force(double u) {
    return glm_lerp(RATTLE_FORCE0, RATTLE_FORCE1,
                    glm_smoothstep(0.0f, 1.0f, (float)(u / RATTLE_GROW)));
}

// The held takes at the cellar door, stopped.
static void hush_door(Haunt* h) {
    const DoorSounds* s = h->house.sounds;
    for (int i = 0; i < DOOR_KNOB_TAKES; i++)
        if (s->knob[i])
            audio_sound_stop(s->knob[i]);
    if (s->rattle)
        audio_sound_stop(s->rattle);
    if (h->house.cellar)
        door_jolt(h->house.cellar, 0.0f);
}

// Whether `feet` stand in the hall, from the front door to the back window.
static bool in_hall(const vec3 feet) {
    return feet[0] > HALL_X0 && feet[0] < HALL_X1 && feet[2] > HOUSE_FRONT_Z &&
           feet[2] < HOUSE_BACK_Z && feet[1] > FLOOR_Y - 0.3f;
}

// Whether a player at `feet` looking along `forward` watches for the slam: in the front of the
// hall, between the front door and the pendant, looking down it toward the cellar door.
static bool watching(const vec3 feet, const vec3 forward) {
    const float across = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    return in_hall(feet) && feet[2] < WATCH_Z1 && across > 1e-4f && forward[2] > WATCH_COS * across;
}

// The house as it was before it began.
static const HauntLive NORMAL = {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f};

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
    h->cue = audio_sound_from_file(audio, "assets/audio/silent/veil.flac", AUDIO_BUS_SFX);
    if (h->cue)
        audio_sound_set_volume(h->cue, CUE_VOLUME);
    // The tries are held at the leaf's hall side, where the knob and the latch are: heard in the
    // hall, and on the cellar stair through the door.
    const DoorSounds* s = house->sounds;
    if (!house->cellar || !house->cellar->entity)
        return;
    for (int i = 0; i <= DOOR_KNOB_TAKES; i++) {
        Sound* take = i < DOOR_KNOB_TAKES ? s->knob[i] : s->rattle;
        if (take)
            audio_sound_set_position(take, house->cellar->entity->position);
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
    if (h->house.filament) {
        glm_vec3_copy(h->house.filament->emissive, n->filament);
        n->filament_nits = h->house.filament->emissive_strength;
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
    // The slam and the creak wait for the player -- unless this begins part-way in, when they are
    // past, and the doors stand where they would by then: the cellar door shut, its tries under way
    // past RATTLE_AT, and the bathroom door ajar if it was shut and shut if it was open. What came
    // before `into` is not played.
    const bool past = into > 0.0;
    h->stepped = time;
    h->watched = past;
    h->hall_at = -1.0;
    h->creaked = past;
    h->tries = HAUNT_TRIES_WAITING;
    Door* cellar = h->house.cellar;
    if (past && cellar) {
        if (!door_is_shut(cellar))
            door_set(cellar, 0.0f, 0.0f);
        if (into >= RATTLE_AT) {
            h->tries = HAUNT_TRIES_ON;
            h->tries_at = h->began + RATTLE_AT;
        }
    }
    Door* bath = h->house.bath;
    if (past && bath) {
        const float to = door_is_shut(bath) ? door_travel_at(bath, CREAK_ANGLE) : 0.0f;
        door_set(bath, to, to);
    }
    // The house goes quiet as its lights die, as a supply failing would have it: the clock stops
    // dead, the fridge's compressor cuts out with a clunk, the tubes and the cellar's bulb go out
    // for as long as it lasts, and the drone starts up from nothing.
    if (h->house.clock)
        clock_stop(h->house.clock, true);
    if (h->house.fridge)
        kitchen_fridge_run(h->house.fridge, false);
    house_supply(h, SUPPLY_OUT);
    if (h->house.lights)
        lights_swing(h->house.lights, PENDANT_SWING, PENDANT_EASE);
    drone_up(h, 0.0f);
    for (int i = 0; i < HAUNT_DRONE_TONES; i++)
        if (h->drone[i])
            audio_sound_play(h->drone[i]);
    // From its start, so only a haunting begun at its own start: one begun part way in is past it.
    if (into <= 0.0)
        audio_sound_play(h->cue);
    if (h->trace)
        printf("haunt begin at %.3f, %.3f s in\n", time, into);
}

void haunt_end(Haunt* h) {
    if (!h->active)
        return;
    apply(h, &NORMAL);
    house_supply(h, 1.0f);
    if (h->house.lights)
        lights_swing(h->house.lights, 0.0f, PENDANT_CAUGHT);
    tint_tv(h, 0.0f, 1.0f);
    if (h->house.tv)
        tv_hiss(h->house.tv, 1.0f);
    grade(h, 0.0f);
    for (int i = 0; i < HAUNT_DRONE_TONES; i++)
        if (h->drone[i])
            audio_sound_stop(h->drone[i]);
    audio_sound_stop(h->cue);
    // A door slamming goes on shutting; the bathroom door stands where it drifted to.
    hush_door(h);
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

// The slam and the creak at sim time `time`: once the player watches for it, a cellar door standing
// open slams shut in front of them; and soon after they first stand in the hall, the bathroom door
// creaks.
static void door_beats(Haunt* h, double time, const vec3 feet, const vec3 forward) {
    Door* cellar = h->house.cellar;
    if (cellar && !h->watched && time - h->began >= WATCH_AFTER && watching(feet, forward)) {
        h->watched = true;
        door_slam(cellar, 0.0f);
        if (h->trace)
            printf("haunt watched at %.3f\n", time - h->began);
    }
    Door* bath = h->house.bath;
    if (h->creaked || !bath)
        return;
    if (h->hall_at < 0.0) {
        if (in_hall(feet))
            h->hall_at = time;
        return;
    }
    if (time - h->hall_at < CREAK_AT)
        return;
    h->creaked = true;
    const bool shut = door_is_shut(bath);
    if (shut)
        door_drift(bath, door_travel_at(bath, CREAK_ANGLE), CREAK_SECONDS);
    else
        door_drift(bath, 0.0f, CREAK_SHUT_SECONDS);
    const DoorSounds* s = h->house.sounds;
    door_hear(s, s->creak, bath, CREAK_VOLUME);
    if (h->trace)
        printf("haunt creak %s at %.3f\n", shut ? "open" : "shut", time - h->began);
}

// The tries at the cellar door, from the step at sim time `before` to the one at `time`: from
// RATTLE_AT into the haunting, once the door stands shut, from their first however late that is,
// until the door is opened on them.
static void cellar_tries(Haunt* h, double time, double before) {
    Door* cellar = h->house.cellar;
    const double u = time - h->began;
    if (!cellar || u < RATTLE_AT || h->tries == HAUNT_TRIES_OVER)
        return;
    const bool shut = door_is_shut(cellar);
    if (h->tries == HAUNT_TRIES_WAITING) {
        if (!shut)
            return;
        h->tries = HAUNT_TRIES_ON;
        h->tries_at = time;
    } else if (!shut) {
        h->tries = HAUNT_TRIES_OVER;
        hush_door(h);
        if (h->trace)
            printf("haunt tries over at %.3f, the cellar door opened\n", u);
        return;
    }
    const DoorSounds* s = h->house.sounds;
    const double r = time - h->tries_at, r_was = before - h->tries_at;
    const DoorTry t = rattle_try(r, h->seed);
    const float force = rattle_force(r);
    if (h->trace && r_was < t.at && t.at <= r)
        printf("haunt try at %.3f, %d turns, %d shakes\n", u, t.turns, t.shakes);
    for (int i = 0; i < t.turns; i++) {
        const double at = t.at + t.turn_at[i];
        Sound* knob = s->knob[t.knob[i]];
        if (r_was < at && at <= r && knob) {
            audio_sound_set_volume(knob, KNOB_VOLUME * force);
            audio_sound_play(knob);
        }
    }
    for (int j = 0; j < t.shakes; j++) {
        const double at = t.at + door_try_shake_at(j);
        if (r_was < at && at <= r && s->rattle) {
            audio_sound_set_volume(s->rattle, RATTLE_VOLUME * force);
            audio_sound_play(s->rattle);
        }
    }
    door_jolt(cellar, RATTLE_PLAY * force * door_try_pull(&t, r));
}

void haunt_step(Haunt* h, double time, const vec3 feet, const vec3 forward) {
    if (!h->active)
        return;
    const double before = h->stepped;
    h->stepped = time;
    door_beats(h, time, feet, forward);
    cellar_tries(h, time, before);
}

void haunt_frame(Haunt* h, double time) {
    if (!h->active)
        return;
    const double u = time - h->began;
    // Out, then up to each circuit's own failing, its episodes counted from when it came back.
    const double back = fmax(0.0, u - BLACKOUT_SECONDS);
    const float up = glm_smoothstep(0.0f, 1.0f, (float)(back / RISE_SECONDS));
    const float on = SUPPLY_OUT + (1.0f - SUPPLY_OUT) * up;
    const float hall = on * failing(back, HALL_PERIOD, h->seed);
    h->live = (HauntLive){
        .hall = hall,
        .lamp = on * failing(back, LAMP_PERIOD, h->seed ^ 0x9e3779b9u),
        .red = 1.0f,
        .dark_out = 1.0f,
        .mains = SUPPLY_OUT,
        // The bounce comes back with the hall's light, failing with it.
        .ambient = hall,
    };
    apply(h, &h->live);
    tint_tv(h, 1.0f, on);
    if (h->house.tv)
        tv_hiss(h->house.tv, up);
    grade(h, 1.0f);
    drone_up(h, glm_smoothstep(0.0f, 1.0f, (float)(u / DRONE_RISE_SECONDS)));
}

void haunt_rest(Haunt* h, bool rest) {
    if (h->active)
        apply(h, rest ? &NORMAL : &h->live);
}
