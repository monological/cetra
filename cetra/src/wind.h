#ifndef _WIND_H_
#define _WIND_H_

#include <cglm/cglm.h>

#include "uniform.h"

struct Scene;

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
    // How far apart in the sway cycle two objects at different places are,
    // in turns. 0 = every object sways in lockstep, which is what a single
    // object wants and what a field of instanced copies of one mesh must not
    // have -- wind is evaluated in object space from per-mesh uniforms, so
    // without this a thousand scattered trees beat as one.
    float phase_variation;
} Wind;

// Created with gentle-draft defaults; adjust fields directly to tune.
Wind* create_wind(const char* name);
void free_wind(Wind* wind);
void set_wind_name(Wind* wind, const char* name);

// Location-guarded upload of the global wind uniforms to a program (mirrors
// shadow_upload_cascade_uniforms). A NULL wind uploads uWindStrength = 0, so
// every wind-aware shader early-outs and nothing moves.
//
// `world_origin` is the scene's accumulated origin shift, and it is uploaded
// whether or not there is wind -- it is the coordinate frame the program reads,
// not part of the wind model. NULL means the origin has never moved. Every
// program that displaces or depth-tests geometry comes through here, which is
// what keeps them from disagreeing about where the world is.
void wind_upload_to_program(const Wind* wind, const vec3 world_origin, UniformManager* u);

// An upper bound, in OBJECT space, on how far windOffset() can move any vertex
// of a mesh with this response and mode -- so a wind-driven mesh can be bounded
// and therefore culled, instead of being exempted from every frustum test.
//
// Object space because that is where the displacement is added (see
// object_position.glsl); the caller's existing transformed-AABB test then
// carries the node's scale, which is also the scale the displacement gets.
//
// `flex_max` and `leaf_max` are the mesh's own measured vertex maxima
// (Mesh.wind_flex_max / wind_leaf_max) and are ignored for mode 0, which reads
// no vertex data. Returns exactly 0 wherever the shader early-outs, so a
// rigid mesh and a windless scene both keep their import bounds untouched.
float wind_max_offset(const Wind* wind, float response, int mode, float flex_max, float leaf_max);

// Prints the largest displacement windOffset can be driven to, beside the bound
// wind_max_offset claims for the same inputs, per wind-responsive mesh, in the
// --water-fft-probe idiom.
//
// It drives the REAL shader through transform feedback rather than a C mirror of
// it. The two halves above share their coefficients (wind_bounds.glsl) and
// cannot share their arithmetic, so a term added to windOffset makes the bound
// non-conservative with nothing to say so -- and the symptom is geometry culled
// while it is on screen, which no frame in the corpus is framed to catch. A C
// port would have checked the bound against a third copy and passed straight
// through exactly that edit.
//
// Needs a live GL context.
void wind_bound_probe(const struct Scene* scene);

#endif // _WIND_H_
