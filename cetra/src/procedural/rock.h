#ifndef _ROCK_H_
#define _ROCK_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../mesh.h"

// A noise-displaced icosphere.
//
// An ICOsphere rather than a UV sphere, and that choice is about LOD rather than
// about looks. A UV sphere's seam meridian splits vertices that are
// geometrically coincident (u wraps 1 -> 0), which meshoptimizer classifies as a
// seam and constrains; its pole fans concentrate degenerate triangles at two
// points. An icosphere has neither -- a closed manifold with every vertex
// shared, congruent faces, and every vertex of degree five or six -- so the
// simplifier can collapse anywhere and a rock gives the cleanest chain here.

typedef struct RockParams {
    float radius;
    // Triangles = 20 * 4^subdivisions. Level 3 is 1280, comfortably over
    // lod.c's 256-triangle floor with room for a full four-level chain.
    int subdivisions;
    float roughness;  // radial displacement as a fraction of radius
    float noise_freq; // lumps per unit over the unit sphere
    unsigned seed;
} RockParams;

RockParams rock_default_params(void);

// Fills positions, normals (recomputed from the displaced surface, not inherited
// from the sphere), tangents and spherical UV0. No vertex colours: a rock is one
// material, and the tint that matters comes from the light.
bool rock_build_mesh(const RockParams* p, Mesh* mesh);

#endif // _ROCK_H_
