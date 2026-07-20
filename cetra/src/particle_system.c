#include "particle_system.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_EMITTER_CAP 4

ParticleSystem* create_particle_system(const char* name) {
    ParticleSystem* s = calloc(1, sizeof(ParticleSystem));
    if (!s)
        return NULL;
    s->name = name ? strdup(name) : NULL;
    return s;
}

void particle_system_set_backend(ParticleSystem* s, ParticleSimBackend* backend) {
    if (!s)
        return;
    if (s->backend && s->backend->free_fn)
        s->backend->free_fn(s->backend);
    s->backend = backend;
}

void particle_system_add_emitter(ParticleSystem* s, ParticleEmitter* e) {
    if (!s || !e)
        return;
    if (s->emitter_count == s->emitter_cap) {
        size_t nc = s->emitter_cap ? s->emitter_cap * 2 : INITIAL_EMITTER_CAP;
        ParticleEmitter** grown = realloc(s->emitters, nc * sizeof(ParticleEmitter*));
        if (!grown)
            return;
        s->emitters = grown;
        s->emitter_cap = nc;
    }
    s->emitters[s->emitter_count++] = e;
}

void particle_system_update(ParticleSystem* s, float dt, float t) {
    if (!s || !s->backend || !s->backend->simulate)
        return;
    for (size_t i = 0; i < s->emitter_count; i++)
        s->backend->simulate(s->backend, s->emitters[i], dt, t);
}

void particle_system_render(ParticleSystem* s, const ParticleRenderContext* ctx) {
    if (!s || !s->backend)
        return;
    for (size_t i = 0; i < s->emitter_count; i++) {
        ParticleEmitter* e = s->emitters[i];
        if (!e->renderer)
            continue;
        ParticleInstanceView view = {0};
        if (s->backend->acquire_instances)
            s->backend->acquire_instances(s->backend, e, &view);
        if (e->renderer->prepare)
            e->renderer->prepare(e->renderer, &view, ctx);
        if (e->renderer->draw)
            e->renderer->draw(e->renderer, &view, ctx);
    }
}

void free_particle_system(ParticleSystem* s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->emitter_count; i++)
        free_emitter(s->emitters[i]);
    free(s->emitters);
    if (s->backend && s->backend->free_fn)
        s->backend->free_fn(s->backend);
    free(s->name);
    free(s);
}
