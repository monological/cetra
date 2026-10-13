#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "haunt.h"
#include "kit.h"
#include "mats.h"

// The hall's light goes out as it begins and comes back red this many seconds later, at HALL_GAIN
// times what it gave: red is the dimmest third of white to the eye, and the hall would read as
// dark rather than red. The glass itself keeps its own nits, or it burns through the tone curve
// to pink.
#define HALL_DARK_SECONDS 0.35
static const vec3 HALL_RED = {1.0f, 0.08f, 0.04f};
#define HALL_GAIN 2.0f
// A failing supply never quite goes out: these lights cast cached shadows, and a cached light at
// nothing leaves its shadow undrawn (basement.c's bulb, spec 13.31).
#define HALL_FLOOR 0.03f
// The set's white as the hall goes red: its red channel lifted so the snow is as bright to the eye
// as a red can be made, its others all but gone.
static const vec3 TV_RED = {1.6f, 0.1f, 0.06f};

// The hall's circuit failing, at `u` seconds in: each period lit a while at a sag, then either a
// dropout -- dead, one catch, dead -- or a stutter in short cells through every level it has.
static float hall_level(double u, unsigned int seed) {
    const double period = 2.2;
    const uint32_t cycle = (uint32_t)floor(u / period);
    const float in = (float)(u - (double)cycle * period);
    const float lit = 0.6f + 1.0f * kit_hash01(cycle, 10u, seed);
    const float sag = 0.8f + 0.2f * kit_hash01(cycle, 11u, seed);
    if (in < lit)
        return sag;
    if (kit_hash01(cycle, 12u, seed) < 0.3f) {
        const float caught = lit + 0.15f + 0.4f * kit_hash01(cycle, 13u, seed);
        return in >= caught && in < caught + 0.05f ? sag : HALL_FLOOR;
    }
    const float h = kit_hash01(cycle, 100u + (uint32_t)((in - lit) / 0.035f), seed);
    return h < 0.4f ? HALL_FLOOR : h < 0.55f ? 0.2f : h < 0.7f ? 0.55f : 1.0f;
}

// The floor lamp's filament come loose, at `u` seconds in: a shimmer just under its own light, and
// once a period a burst of stutter.
static float lamp_level(double u, unsigned int seed) {
    const double period = 2.3;
    const uint32_t cycle = (uint32_t)floor(u / period);
    const float in = (float)(u - (double)cycle * period);
    const float start = 0.2f + 1.4f * kit_hash01(cycle, 20u, seed);
    const float len = 0.15f + 0.5f * kit_hash01(cycle, 21u, seed);
    if (in >= start && in < start + len) {
        const float h = kit_hash01(cycle, 200u + (uint32_t)((in - start) / 0.025f), seed);
        return h < 0.4f ? 0.06f : h < 0.6f ? 0.45f : 1.0f;
    }
    return 0.92f + 0.08f * kit_hash01((uint32_t)floor(u / 0.1), 22u, seed);
}

// The house's lights at a hall and a lamp supply, the hall `red` of the way to red.
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
}

// The set's white `red` of the way to red. Not part of apply: the set rests itself (tv_rest).
static void tint_tv(Haunt* h, float red) {
    if (!h->house.tv)
        return;
    vec3 tint = {1.0f, 1.0f, 1.0f};
    glm_vec3_lerp(tint, (float*)TV_RED, red, tint);
    tv_tint(h->house.tv, tint);
}

// The house as it was before it began.
static const HauntLive NORMAL = {1.0f, 1.0f, 0.0f};

void haunt_start(Haunt* h, const HauntHouse* house, unsigned int seed) {
    *h = (Haunt){.house = *house, .seed = seed, .shade_slot = -1};
    if (!house->scene)
        return;
    h->hall[0] = scene_find_light(house->scene, "hall_bulb");
    h->hall[1] = scene_find_light(house->scene, "hall_lantern");
    h->lamp = scene_find_light(house->scene, "floor_lamp");
    if (h->lamp)
        h->shade_slot = mats_shade_slot(house->shade, h->lamp->original_position);
    if (!h->hall[0] || !h->hall[1] || !h->lamp || h->shade_slot < 0)
        fprintf(stderr, "silent: the haunting finds%s%s%s%s\n", h->hall[0] ? "" : " no hall bulb",
                h->hall[1] ? "" : " no hall lantern", h->lamp ? "" : " no floor lamp",
                h->shade_slot >= 0 ? "" : " no lampshade bulb");
}

void haunt_begin(Haunt* h, double time, double into) {
    if (h->active)
        return;
    // What the lights stand at now, which is what they go back to.
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
        .hall = dark ? HALL_FLOOR : hall_level(u, h->seed),
        .lamp = lamp_level(u, h->seed ^ 0x9e3779b9u),
        .red = dark ? 0.0f : 1.0f,
    };
    apply(h, &h->live);
    tint_tv(h, h->live.red);
}

void haunt_rest(Haunt* h, bool rest) {
    if (h->active)
        apply(h, rest ? &NORMAL : &h->live);
}
