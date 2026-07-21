#include "particle_system.h"
#include "scene.h" // SceneNode->global_transform for the spawn frame

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
        if (!grown) {
            free_particle_emitter(e); // ownership was transferred to us
            return;
        }
        s->emitters = grown;
        s->emitter_cap = nc;
    }
    s->emitters[s->emitter_count++] = e;
}

size_t particle_system_live_count(const ParticleSystem* s) {
    if (!s)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < s->emitter_count; i++)
        n += s->emitters[i]->pool->count;
    return n;
}

void particle_system_update(ParticleSystem* s, float dt, float t) {
    if (!s || !s->backend || !s->backend->simulate)
        return;
    for (size_t i = 0; i < s->emitter_count; i++) {
        ParticleEmitter* e = s->emitters[i];
        // The node's world transform is the emitter's spawn frame (identity if
        // unattached). global_transform is last render frame's -- one frame of
        // spawn-origin latency, fine for static/slow emitters.
        if (s->node)
            glm_mat4_copy(s->node->global_transform, e->local_to_world);
        s->backend->simulate(s->backend, e, dt, t);
    }
}

void particle_system_render(ParticleSystem* s, const ParticleRenderContext* ctx) {
    if (!s || !s->backend)
        return;
    // Per emitter: acquire -> prepare -> draw, kept paired on purpose. The CPU
    // backend hands back a view into ONE shared staging buffer, so the next
    // emitter's acquire overwrites it; prepare uploads to that emitter's own VBO
    // before we move on. A future "acquire all, then draw all" refactor would
    // need per-emitter staging.
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
        free_particle_emitter(s->emitters[i]);
    free(s->emitters);
    if (s->backend && s->backend->free_fn)
        s->backend->free_fn(s->backend);
    free(s->name);
    free(s);
}
