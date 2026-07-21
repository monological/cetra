#include "particle_emitter.h"
#include "particle_renderer.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_MODULE_CAP 4
#define EMITTER_RNG_SEED   0x9E3779B9u

ParticleEmitter* create_particle_emitter(const char* name, size_t capacity) {
    ParticleEmitter* e = calloc(1, sizeof(ParticleEmitter));
    if (!e)
        return NULL;

    e->name = name ? strdup(name) : NULL;
    e->pool = create_particle_pool(capacity);
    if (!e->pool) {
        free(e->name);
        free(e);
        return NULL;
    }
    e->rng_state = EMITTER_RNG_SEED;
    glm_mat4_identity(e->local_to_world);
    return e;
}

static void free_module_list(ParticleModule** list, size_t n) {
    for (size_t i = 0; i < n; i++)
        free_particle_module(list[i]);
    free(list);
}

void free_particle_emitter(ParticleEmitter* e) {
    if (!e)
        return;
    free_module_list(e->spawn, e->n_spawn);
    free_module_list(e->init, e->n_init);
    free_module_list(e->update, e->n_update);
    if (e->renderer && e->renderer->free_fn)
        e->renderer->free_fn(e->renderer);
    free_particle_pool(e->pool);
    free(e->name);
    free(e);
}

static void list_push(ParticleModule*** list, size_t* n, size_t* cap, ParticleModule* m) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : INITIAL_MODULE_CAP;
        ParticleModule** grown = realloc(*list, nc * sizeof(ParticleModule*));
        if (!grown) {
            free_particle_module(m); // ownership was transferred to us
            return;
        }
        *list = grown;
        *cap = nc;
    }
    (*list)[(*n)++] = m;
}

void particle_emitter_add_module(ParticleEmitter* e, ParticleModule* m) {
    if (!e || !m)
        return;
    switch (m->phase) {
        case PARTICLE_PHASE_SPAWN:
            list_push(&e->spawn, &e->n_spawn, &e->cap_spawn, m);
            break;
        case PARTICLE_PHASE_INIT:
            list_push(&e->init, &e->n_init, &e->cap_init, m);
            break;
        case PARTICLE_PHASE_UPDATE:
            list_push(&e->update, &e->n_update, &e->cap_update, m);
            break;
    }
}

void particle_emitter_set_renderer(ParticleEmitter* e, struct ParticleRenderer* r) {
    if (!e)
        return;
    if (e->renderer && e->renderer->free_fn)
        e->renderer->free_fn(e->renderer);
    e->renderer = r;
}

float particle_emitter_rand01(ParticleEmitter* e) {
    uint32_t x = e->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    e->rng_state = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}
