#ifndef _SILENT_HAUNT_H_
#define _SILENT_HAUNT_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/material.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"

#include "clock.h"
#include "door.h"
#include "kitchen.h"
#include "tv.h"

/*
 * The haunting (spec 13.52), after P.T.: the player's house turns on them. The hall's light fails
 * and comes back red, the living room's lamp stutters and the snow on the television goes red;
 * the clock stops, the fridge cuts out and the house goes quiet under a low drone; something
 * climbs the cellar stair, the bathroom door creaks ajar, and the cellar door is tried from the
 * other side -- until the player comes near it, when everything at the door stops dead.
 *
 * One module with a beginning and an end, so a story can start it where a key does now. Every
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
    Door* cellar;      // the basement door
    Door* bath;        // the bathroom door
    Material* frosted; // the hall lanterns' glass, which nothing else wears
    Material* shade;   // the lampshades' cloth, the floor lamp's bulb one of its own
} HauntHouse;

// What the house's lights were before it began, put back when it ends and held while captured.
typedef struct HauntNormal {
    vec3 hall_colour[2];
    float hall_candela[2];
    vec3 glass;
    float glass_nits;
    float lamp_candela;
} HauntNormal;

// What the last frame drove them at: the hall's and the lamp's supply as a share of their own,
// and how far the hall has gone red.
typedef struct HauntLive {
    float hall;
    float lamp;
    float red;
} HauntLive;

typedef struct Haunt {
    HauntHouse house;
    Light* hall[2]; // the pendant over the hall and the lantern on its west wall
    Light* lamp;    // the living room's floor lamp
    int shade_slot; // its bulb in the shade's
    unsigned int seed;
    bool active;
    double began; // the sim time it began at
    HauntNormal normal;
    HauntLive live;
    bool trace; // say each beat as it comes
} Haunt;

// Takes hold of the house; nothing happens until it begins.
void haunt_start(Haunt* h, const HauntHouse* house, unsigned int seed);
// Begins at sim time `time`, `into` seconds of it already gone by; nothing when it already has.
void haunt_begin(Haunt* h, double time, double into);
// Ends, everything it drove put back as it was before; nothing when it has not begun.
void haunt_end(Haunt* h);
bool haunt_active(const Haunt* h);
// Per frame at sim time `time`: the lights, the set, the picture.
void haunt_frame(Haunt* h, double time);
// For the light's captures (spec 13.42): `rest` holds the house as it was before it began, so no
// capture keeps any of it; false puts back what the last frame drove.
void haunt_rest(Haunt* h, bool rest);

#endif // _SILENT_HAUNT_H_
