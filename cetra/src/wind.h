#ifndef _WIND_H_
#define _WIND_H_

#include <cglm/cglm.h>

#include "uniform.h"

// A first-class, scene-owned wind field. Mirrors how UE's WindDirectionalSource
// and Unity's WindZone are scene objects: the wind carries a direction, strength,
// and gust, and wind-responsive materials sample it (Material.wind_response) to
// displace their vertices in the shader (World-Position Offset, pbr_vert.glsl).
// A single dominant directional wind is an environmental property of the Scene,
// like the sky/IBL -- not a per-node citizen.
typedef enum WindType {
    WIND_DIRECTIONAL = 0,
} WindType;

typedef struct Wind {
    char* name;
    WindType type;
    vec3 direction;       // world-space blow direction (need not be normalized;
                          // the shader normalizes)
    float strength;       // base displacement magnitude (world units)
    float speed;          // sway/ripple advection speed
    float gust_frequency; // gust swell rate; low = occasional gusts
    float gust_amount;    // gust envelope depth 0..1; high = calm between gusts
    float turbulence;     // high-frequency lateral flutter 0..1
} Wind;

// Created with gentle-draft defaults; adjust fields directly to tune.
Wind* create_wind(const char* name);
void free_wind(Wind* wind);
void set_wind_name(Wind* wind, const char* name);

// Location-guarded upload of the global wind uniforms to a program (mirrors
// shadow_upload_cascade_uniforms). A NULL wind uploads uWindStrength = 0, so
// every wind-aware shader early-outs and nothing moves.
void wind_upload_to_program(const Wind* wind, UniformManager* u);

#endif // _WIND_H_
