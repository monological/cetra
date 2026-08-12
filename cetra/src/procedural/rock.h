#ifndef _ROCK_H_
#define _ROCK_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../mesh.h"

// A noise-displaced icosphere.
//
// An ICOsphere rather than a UV sphere, and that choice is about LOD rather than
// about looks. meshoptimizer locks mesh borders, and a UV sphere has two: the
// pole fans and the seam meridian where u wraps 1 -> 0, which splits vertices
// that are geometrically coincident. An icosphere has neither -- it is a closed
// manifold with every vertex shared -- so the simplifier can collapse anywhere
// and a rock produces the cleanest LOD chain in the engine.
//
// Which is the point: leaf cards and grass blades are nearly all border and
// refuse to simplify at all, so a scene needs geometry of this class before a
// LOD chain can demonstrate anything.

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
