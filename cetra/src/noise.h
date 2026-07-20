#ifndef _NOISE_H_
#define _NOISE_H_

#include <cglm/cglm.h>

// Engine noise utilities. The 3D value/Perlin + divergence-free curl field
// drive organic particle turbulence (the 2D Perlin/fbm in apps/tree predates
// this; this is the shared, 3D-capable home). See specs/5.0-particle-system.md.

// Re-seed the permutation table (deterministic). Called lazily with a default
// seed on first use if never called.
void noise_seed(unsigned int seed);

// Classic 3D Perlin ("improved noise"), output roughly in [-1, 1].
float noise_perlin3(float x, float y, float z);

// Fractal Brownian motion over noise_perlin3.
float noise_fbm3(float x, float y, float z, int octaves, float persistence);

// Divergence-free (incompressible) 3D curl field, ~unit magnitude. Ideal for
// swirly, volume-preserving advection of floating particles. Writes to `out`.
void noise_curl3(float x, float y, float z, vec3 out);

#endif // _NOISE_H_
