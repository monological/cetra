#ifndef _SILENT_STREET_H_
#define _SILENT_STREET_H_

#include <stdbool.h>

#include "cetra/scene.h"

#include "houses.h"
#include "kit.h"
#include "layout.h"

// Where every lot's house stands, each side from the west: ours among our side's, and on a lot
// with none a plot of no width, which street_lot_vacant says.
typedef struct StreetPlots {
    HousePlot near[NEAR_LOTS];
    HousePlot far[TERRACE_LOTS];
} StreetPlots;

static inline bool street_lot_vacant(const HousePlot* plot) {
    return plot->x1 <= plot->x0;
}

// Everything outside: the road, its kerbs and sidewalks, our yards, the
// neighbours' houses, the lamps (and their light, at night), the poles and
// their wires, a car -- and the fog that fills the street and stops at
// the house, unless not `fogged`.
void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, bool fogged,
                  StreetPlots* plots);

// A street lamp standing at (x, y, z), its arm out along `yaw`'s +z, lit at night unless dead.
// Returns its light, or NULL when it has none.
Light* street_lamp(Kit* kit, Scene* scene, float x, float y, float z, float yaw, bool night,
                   bool dead, int profile);

// The street lamps' IES profile in the scene's library, or -1 by day or when it will not load.
int street_lamp_profile(Scene* scene, bool night);

// A lamp on a failing ballast: all of its light, or a glimmer for a beat now and then. Lamps of
// different salts fail out of step.
typedef struct FailingLamp {
    Light* light;    // NULL by day, or for a lamp with no light
    float intensity; // what it gives when it is not failing
    unsigned int salt;
} FailingLamp;

// `light` failing from now on; NULL leaves a lamp that does nothing.
FailingLamp failing_lamp(Light* light, unsigned int salt);
// Its light at `time` seconds.
void failing_lamp_update(const FailingLamp* lamp, double time);

// Ground from x0 to x1 and z0 to z1, a box GROUND_DEPTH deep with its top at `top`.
void street_ground(Kit* kit, int mat, float x0, float x1, float z0, float z1, float top);

// A boxy sedan at (x, z) on the road, its length along X, its windows dark: in the street's car
// paint, or a police car's black and white with its light bar dead on the roof.
void street_car(Kit* kit, float x, float z, bool police);

#endif // _SILENT_STREET_H_
