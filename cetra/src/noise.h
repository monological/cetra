#ifndef _NOISE_H_
#define _NOISE_H_

#include <cglm/cglm.h>

// Engine noise utilities: 3D Perlin + a divergence-free curl field that drive
// organic particle turbulence. (apps/tree carries its own separate 2D Perlin;
// consolidating the two is a follow-up.) See specs/5.0-particle-system.md.

// Re-seed the permutation table (deterministic). Called lazily with a default
// seed on first use if never called.
void noise_seed(unsigned int seed);

// Classic 3D Perlin ("improved noise"), output roughly in [-1, 1].
float noise_perlin3(float x, float y, float z);

// Divergence-free (incompressible) 3D curl field, ~unit magnitude. Ideal for
// swirly, volume-preserving advection of floating particles. Writes to `out`.
void noise_curl3(float x, float y, float z, vec3 out);

// Local-table variants for bakes that must not depend on the global table
// above: apps re-seed noise_seed() at init, so anything sampled through the
// global table silently changes with app seeding order, and the lazy seed is
// a data race under threads. A NoisePerm is read-only after init, so N bake
// threads can share one safely.
typedef struct NoisePerm {
    int p[512];
} NoisePerm;

// Deterministic Fisher-Yates from an internal LCG -- never srand()/rand()
// (process-global state, not thread-safe, and stomped by other seeders).
void noise_perm_init(NoisePerm* t, unsigned int seed);

// Tiling 3D Perlin over a lattice that repeats every `period` cells on each
// axis (period <= 256). Sampling x in [0, period) tiles seamlessly.
float noise_perlin3_tiled(const NoisePerm* t, float x, float y, float z, int period);

// Stateless tiling 3D Worley (cellular) F1 distance, ~[0, 1.2]: one feature
// point per unit cell, cell ids wrapped mod `period` so opposite faces share
// features. Pure function of (x, y, z, period, seed) -- thread-safe, and
// independent of every permutation table.
float noise_worley3(float x, float y, float z, int period, unsigned int seed);

#endif // _NOISE_H_
