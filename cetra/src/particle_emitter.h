#ifndef _PARTICLE_EMITTER_H_
#define _PARTICLE_EMITTER_H_

#include <stdint.h>
#include <cglm/cglm.h>

#include "particle_pool.h"
#include "particle_module.h"

struct ParticleRenderer;

// An emitter is DATA: a pool, three phase-ordered module lists, a deterministic
// RNG, and an owned renderer. It has no tick of its own -- the sim backend owns
// the stepping (see particle_sim.h). See specs/5.0-particle-system.md.
typedef struct ParticleEmitter {
    char* name;
    ParticlePool* pool;

    ParticleModule** spawn;
    size_t n_spawn, cap_spawn;
    ParticleModule** init;
    size_t n_init, cap_init;
    ParticleModule** update;
    size_t n_update, cap_update;

    float spawn_request; // written by spawn modules, consumed + cleared by the backend
    uint32_t rng_state;  // per-emitter xorshift32 (deterministic)

    mat4 local_to_world; // emitter->world spawn frame (synced from the attached node each tick)

    struct ParticleRenderer* renderer; // owned; may be NULL

    // Backend-owned GPU scratch (e.g. the transform-feedback backend's ping-pong
    // state buffers + VAOs). NULL for the CPU backend. Freed via sim_state_free
    // in free_particle_emitter -- mirrors the renderer's impl/free_fn idiom.
    void* sim_state;
    void (*sim_state_free)(void* sim_state);
} ParticleEmitter;

ParticleEmitter* create_particle_emitter(const char* name, size_t capacity);
void free_particle_emitter(ParticleEmitter* e);

// Append a module to its phase list (routed by m->phase). The emitter takes
// ownership of the module.
void particle_emitter_add_module(ParticleEmitter* e, ParticleModule* m);

// Assign the emitter's renderer (takes ownership; frees any previous one).
void particle_emitter_set_renderer(ParticleEmitter* e, struct ParticleRenderer* r);

// Deterministic per-emitter random in [0,1), advancing rng_state.
float particle_emitter_rand01(ParticleEmitter* e);

#endif // _PARTICLE_EMITTER_H_
