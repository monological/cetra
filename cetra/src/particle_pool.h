#ifndef _PARTICLE_POOL_H_
#define _PARTICLE_POOL_H_

#include <stddef.h>
#include <cglm/cglm.h>

// A bounded, data-oriented (struct-of-arrays) particle store. Live particles
// are kept compact in [0, count); spawning appends, killing swaps-with-last.
// The pool knows nothing about modules, GL, or emitters -- it is pure storage.
// See specs/5.0-particle-system.md.
typedef struct ParticlePool {
    size_t capacity;
    size_t count;
    vec3* position;
    vec3* velocity;
    vec4* color; // linear HDR rgba (a = base opacity)
    float* size;
    float* rotation;
    float* age;
    float* lifetime;
    float* seed; // per-particle random [0,1) for stable variation
} ParticlePool;

ParticlePool* create_particle_pool(size_t capacity);
void free_particle_pool(ParticlePool* pool);

// Append up to n particles (clamped to remaining capacity), zeroing their
// slots. Returns the index of the first new particle (== the old count); the
// number actually spawned is (pool->count - returned_index).
size_t particle_pool_spawn(ParticlePool* pool, size_t n);

// Kill particle i by swapping the last live particle into its slot (order is
// not preserved). i must be < count.
void particle_pool_kill_swap(ParticlePool* pool, size_t i);

#endif // _PARTICLE_POOL_H_
