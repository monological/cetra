#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cetra/program.h"
#include "cetra/shader_params.h"

#include "haunt.h"
#include "kit.h"
#include "mats.h"
#include "silent_shaders.h"

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
    if (h->trace)
        printf("haunt begin at %.3f, %.3f s in\n", time, into);
}

void haunt_end(Haunt* h) {
    if (!h->active)
        return;
    apply(h, &NORMAL);
    tint_tv(h, 0.0f);
    grade(h, 0.0f);
    h->active = false;
    if (h->trace)
        printf("haunt end\n");
}

bool haunt_active(const Haunt* h) {
    return h->active;
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
}

void haunt_rest(Haunt* h, bool rest) {
    if (h->active)
        apply(h, rest ? &NORMAL : &h->live);
}
