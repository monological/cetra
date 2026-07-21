#include "particle_sim.h"
#include "particle_emitter.h"

#include <stdlib.h>

// CPU sim backend: embeds the base vtable as its first member so the vtable
// pointer and the concrete backend are the same address.
typedef struct {
    ParticleSimBackend base;
    ParticleInstanceData* staging; // packed each frame for acquire_instances
    size_t staging_cap;
} CpuSimBackend;

static void cpu_simulate(ParticleSimBackend* b, ParticleEmitter* e, float dt, float t) {
    (void)b;
    ParticlePool* pool = e->pool;

    // 1-3. SPAWN, then init the new slice (both shared with every backend so the
    //       contract lives in one place). init_slice gives the new particles a
    //       clean slate and runs the INIT modules over them.
    size_t want = particle_emitter_run_spawn(e, dt, t);
    size_t old_count = pool->count;
    particle_pool_spawn(pool, want);
    particle_emitter_init_slice(e, old_count, pool->count, dt, t);

    // 4. UPDATE modules over all live particles (they never kill).
    for (size_t i = 0; i < e->n_update; i++)
        e->update[i]->run(e->update[i], e, 0, pool->count, dt, t);

    // 5. Advance age, then compact backwards so a swap-remove never moves an
    //    unprocessed particle into an already-skipped slot. Kill lives here only.
    for (size_t i = 0; i < pool->count; i++)
        pool->age[i] += dt;
    for (size_t i = pool->count; i-- > 0;) {
        if (pool->age[i] >= pool->lifetime[i])
            particle_pool_kill_swap(pool, i);
    }
}

static void cpu_acquire_instances(ParticleSimBackend* b, ParticleEmitter* e,
                                  ParticleInstanceView* out) {
    CpuSimBackend* self = (CpuSimBackend*)b;
    ParticlePool* pool = e->pool;

    if (pool->count > self->staging_cap) {
        // Grow to full capacity so this never reallocs mid-run.
        ParticleInstanceData* grown =
            realloc(self->staging, pool->capacity * sizeof(ParticleInstanceData));
        if (!grown) {
            out->count = 0;
            out->cpu_instances = NULL;
            out->gpu_instance_vbo = 0;
            return;
        }
        self->staging = grown;
        self->staging_cap = pool->capacity;
    }

    for (size_t i = 0; i < pool->count; i++) {
        ParticleInstanceData* d = &self->staging[i];
        glm_vec3_copy(pool->position[i], d->center);
        d->_pad0 = 0.0f;
        d->params[0] = pool->size[i];
        d->params[1] = pool->rotation[i];
        d->params[2] = pool->lifetime[i] > 0.0f ? pool->age[i] / pool->lifetime[i] : 0.0f;
        d->params[3] = pool->seed[i];
        glm_vec4_copy(pool->color[i], d->color);
    }

    out->count = pool->count;
    out->cpu_instances = self->staging;
    out->gpu_instance_vbo = 0;
}

static size_t cpu_live_count(ParticleSimBackend* b, ParticleEmitter* e) {
    (void)b;
    return e->pool ? e->pool->count : 0;
}

static void cpu_free(ParticleSimBackend* b) {
    CpuSimBackend* self = (CpuSimBackend*)b;
    free(self->staging);
    free(self);
}

ParticleSimBackend* create_cpu_particle_sim_backend(void) {
    CpuSimBackend* self = calloc(1, sizeof(CpuSimBackend));
    if (!self)
        return NULL;
    self->base.name = "cpu";
    self->base.simulate = cpu_simulate;
    self->base.acquire_instances = cpu_acquire_instances;
    self->base.live_count = cpu_live_count;
    self->base.free_fn = cpu_free;
    return &self->base;
}
