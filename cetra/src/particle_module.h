#ifndef _PARTICLE_MODULE_H_
#define _PARTICLE_MODULE_H_

#include <stddef.h>
#include <cglm/cglm.h>

// A ParticleModule is one composable behavior in an emitter's stack. Modules
// are fn-pointer descriptors with module-owned opaque params, so a new module
// lives entirely in its own translation unit and needs zero changes to the
// emitter/backend core (the Niagara model). See specs/5.0-particle-system.md.
//
// The [begin,end) slice each kernel receives is deliberately shaped like a GPU
// compute invocation range, so the module boundary survives a future CPU->GPU
// backend swap.

struct ParticleEmitter;
struct ParticleModule;

typedef enum {
    PARTICLE_PHASE_SPAWN, // decides how many to spawn this step (writes emitter->spawn_request)
    PARTICLE_PHASE_INIT,  // stamps newly spawned particles [begin,end)
    PARTICLE_PHASE_UPDATE // advances all live particles [begin,end) = [0,count)
} ParticleModulePhase;

// Phase-appropriate kernel. SPAWN ignores [begin,end); INIT sees the new slice;
// UPDATE sees all live particles. dt is the fixed timestep, t the total time.
typedef void (*ParticleModuleFn)(struct ParticleModule* m, struct ParticleEmitter* e, size_t begin,
                                 size_t end, float dt, float t);

typedef struct ParticleModule {
    const char* name;
    ParticleModulePhase phase;
    ParticleModuleFn run;
    void (*free_fn)(struct ParticleModule* m);
    void* params; // concrete type private to the module's .c
} ParticleModule;

void free_particle_module(ParticleModule* m);

// --- Concrete v1 modules (spore demo needs these) ---

// SPAWN: emit `rate` particles per second (fractional remainder accumulated so
// rates below 1/dt still spawn).
ParticleModule* particle_module_spawn_rate(float rate);
void particle_module_spawn_rate_set(ParticleModule* m, float rate);

// INIT: uniformly random position inside the AABB [min,max].
ParticleModule* particle_module_init_box_location(vec3 min, vec3 max);
// INIT: lifetime uniform in [min_seconds, max_seconds].
ParticleModule* particle_module_init_lifetime(float min_seconds, float max_seconds);
// INIT: billboard half-size uniform in [min,max].
ParticleModule* particle_module_init_size(float min_size, float max_size);
// INIT: base HDR color, each rgb channel jittered by +/- rgb_jitter.
ParticleModule* particle_module_init_color(vec4 base_rgba, float rgb_jitter);

// UPDATE: divergence-free curl-noise turbulence advecting velocity. `scale` is
// the spatial frequency, `strength` the acceleration magnitude, `timescale` the
// rate the noise domain drifts over time (organic, non-repeating swirl).
ParticleModule* particle_module_update_curl_noise(float scale, float strength, float timescale);
void particle_module_curl_set_strength(ParticleModule* m, float strength);

// UPDATE: add a constant acceleration (m/s^2), e.g. faint upward buoyancy.
ParticleModule* particle_module_update_drift(vec3 accel);
// UPDATE: integrate position by velocity and apply a per-step velocity damping
// factor `drag` in (0,1].
ParticleModule* particle_module_update_integrate(float drag);

#endif // _PARTICLE_MODULE_H_
