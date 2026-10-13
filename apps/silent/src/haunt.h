#ifndef _SILENT_HAUNT_H_
#define _SILENT_HAUNT_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/postfx.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"

#include "basement.h"
#include "clock.h"
#include "door.h"
#include "kitchen.h"
#include "lights.h"
#include "tv.h"

/*
 * The haunting (spec 13.52), after P.T.: the player's house turns on them. The power goes -- every
 * light in the house, the night outside, the windows black -- and the hall's light comes back red,
 * failing; the living room's lamp falters, the snow on the television goes red, and the picture is
 * drained to red. The clock stops, the fridge cuts out and the house goes quiet under a low drone;
 * the bathroom door creaks ajar -- or shut, if it stood open -- and the cellar door is tried hard
 * from the other side, on and on, until it is ended: the house's story ends it as the player opens
 * that door (spec 13.53).
 *
 * One module with a beginning and an end, which the house's story drives (story.h). Every
 * beat is a pure function of the seconds since it began and a seed, so a headless run shows the
 * same frame and plays the same sound every time; and nothing of it is saved -- the doors it moves
 * keep their own state, which is.
 */

// What the haunting takes hold of, borrowed; any may be NULL, and what it would drive is left be.
typedef struct HauntHouse {
    Engine* engine;
    Scene* scene;
    AudioSystem* audio;
    Clock* clock;
    Tv* tv;
    Fridge* fridge;
    Lights* lights;       // the kitchen's tubes
    Basement* basement;   // the cellar stair's bulb
    Door* cellar;         // the basement door
    Door* bath;           // the bathroom door
    Material* filament;   // the hall lanterns' bulbs' filaments, which nothing else wears
    Material* shade;      // the lampshades' cloth, each lamp's bulb one of its own
    Material* bulbs;      // the bare bulbs' glass, the bathroom's among them
    Material* panes;      // the house's clear window glass
    Material* lamp_heads; // the street lamps' glowing heads
} HauntHouse;

// The street lamps it puts out, at most.
#define HAUNT_STREET_LAMPS 32
// The tones the drone is made of, and the knob's tries.
#define HAUNT_DRONE_TONES 4
#define HAUNT_KNOB_TAKES  4

// What the house was before it began, put back when it ends and held while captured.
typedef struct HauntNormal {
    vec3 hall_colour[2];
    float hall_candela[2];
    vec3 filament;
    float filament_nits;
    float lamp_candela;
    float bath_candela;
    float bedside_candela;
    float bulb_nits;
    vec3 panes;
    float lamp_head_nits;
    float street_candela[HAUNT_STREET_LAMPS];
    float sky; // the environment's strength
    // The picture's finishing, which its grade takes over, and the fog's glow, which it puts out.
    bool vignette;
    float vignette_strength, vignette_radius;
    bool grain;
    float grain_strength;
    vec3 fog_ambient;
    bool fog_from_sky;
} HauntNormal;

// What the last frame drove the lights at: the hall's and the lamp's supply as a share of their
// own, how far the hall has gone red, whether the night outside is gone, the share of its supply
// every other light in the house gets, and the share of its own the light from no lamp gets -- the
// bounce and the reflections the house was captured in, and the sky's.
typedef struct HauntLive {
    float hall;
    float lamp;
    float red;
    float dark_out;
    float mains;
    float ambient;
} HauntLive;

typedef struct Haunt {
    HauntHouse house;
    Light* hall[2]; // the pendant over the hall and the lantern on its west wall
    Light* lamp;    // the living room's floor lamp
    int shade_slot; // its bulb in the shade's
    Light* bath;    // the bathroom's bare bulb
    Light* bedside; // the bedroom's lamp
    int bedside_slot;
    Light* street[HAUNT_STREET_LAMPS];
    int street_count;
    PostFXPass* grade;               // drains the picture to red; off while it is not haunted
    Sound* drone[HAUNT_DRONE_TONES]; // NULL without audio
    // What the doors sound like, each NULL without audio: the creak and the slam are
    // fire-and-forget, the knob and the rattle held, so the end can cut them dead.
    Sound* creak;
    Sound* slam;
    Sound* knob[HAUNT_KNOB_TAKES];
    Sound* rattle;
    unsigned int seed;
    bool active;
    double began; // the sim time it began at
    // The door beats wait for the player to watch for them, from the front of the hall down it:
    // whether they have, the sim time the beats began at then, and the seconds into them the last
    // step reached, the beats since firing in the next. The cellar door, standing open when the
    // beats begin, is slamming shut.
    bool watched;
    double doors_at;
    double last_u;
    bool slamming;
    HauntNormal normal;
    HauntLive live;
    bool trace; // say each beat as it comes
} Haunt;

// Takes hold of the house, and adds the picture's grade to the post chain, off; nothing happens
// until it begins.
void haunt_start(Haunt* h, const HauntHouse* house, unsigned int seed);
// Begins at sim time `time`; nothing when it already has. `into` above 0 begins it that many
// seconds in, its door beats with it rather than waiting to be watched for.
void haunt_begin(Haunt* h, double time, double into);
// Ends, everything it drove put back as it was before; nothing when it has not begun.
void haunt_end(Haunt* h);
bool haunt_active(const Haunt* h);
// Per fixed step at sim time `time`, after the doors have moved, the player's feet at `feet` and
// looking along `forward`: whether they watch for the door beats yet, then the slam, the creak and
// the tries.
void haunt_step(Haunt* h, double time, const vec3 feet, const vec3 forward);
// Per frame at sim time `time`: the lights, the windows, the set, the picture, the drone.
void haunt_frame(Haunt* h, double time);
// For the light's captures (spec 13.42): `rest` holds the house as it was before it began, so no
// capture keeps any of it; false puts back what the last frame drove.
void haunt_rest(Haunt* h, bool rest);

#endif // _SILENT_HAUNT_H_
