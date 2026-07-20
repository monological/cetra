#include "particle_module.h"
#include "particle_emitter.h"
#include "noise.h"

#include <math.h>
#include <stdlib.h>

// Every v1 module frees just its params + itself (name is a string literal).
static void module_default_free(ParticleModule* m) {
    if (!m)
        return;
    free(m->params);
    free(m);
}

static ParticleModule* module_new(const char* name, ParticleModulePhase phase, ParticleModuleFn run,
                                  void* params) {
    ParticleModule* m = calloc(1, sizeof(ParticleModule));
    if (!m) {
        free(params);
        return NULL;
    }
    m->name = name;
    m->phase = phase;
    m->run = run;
    m->free_fn = module_default_free;
    m->params = params;
    return m;
}

void free_particle_module(ParticleModule* m) {
    if (!m)
        return;
    if (m->free_fn)
        m->free_fn(m);
    else
        free(m);
}

// --- SPAWN: rate ---

typedef struct {
    float rate;
    float acc; // fractional remainder so sub-1/dt rates still spawn
} SpawnRateParams;

static void spawn_rate_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                           float dt, float t) {
    (void)begin;
    (void)end;
    (void)t;
    SpawnRateParams* p = m->params;
    p->acc += p->rate * dt;
    float whole = floorf(p->acc);
    p->acc -= whole;
    e->spawn_request += whole;
}

ParticleModule* particle_module_spawn_rate(float rate) {
    SpawnRateParams* p = calloc(1, sizeof(SpawnRateParams));
    if (!p)
        return NULL;
    p->rate = rate;
    return module_new("spawn_rate", PARTICLE_PHASE_SPAWN, spawn_rate_run, p);
}

void particle_module_spawn_rate_set(ParticleModule* m, float rate) {
    if (m && m->params)
        ((SpawnRateParams*)m->params)->rate = rate;
}

// --- INIT: box location ---

typedef struct {
    vec3 min, max;
} BoxLocParams;

static void init_box_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                         float t) {
    (void)dt;
    (void)t;
    BoxLocParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        for (int k = 0; k < 3; k++)
            e->pool->position[i][k] =
                p->min[k] + (p->max[k] - p->min[k]) * particle_emitter_rand01(e);
    }
}

ParticleModule* particle_module_init_box_location(vec3 min, vec3 max) {
    BoxLocParams* p = calloc(1, sizeof(BoxLocParams));
    if (!p)
        return NULL;
    glm_vec3_copy(min, p->min);
    glm_vec3_copy(max, p->max);
    return module_new("init_box_location", PARTICLE_PHASE_INIT, init_box_run, p);
}

// --- INIT: scalar range (shared by lifetime and size) ---

typedef struct {
    float min, max;
} RangeParams;

static void init_lifetime_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                              float dt, float t) {
    (void)dt;
    (void)t;
    const RangeParams* p = m->params;
    for (size_t i = begin; i < end; i++)
        e->pool->lifetime[i] = p->min + (p->max - p->min) * particle_emitter_rand01(e);
}

ParticleModule* particle_module_init_lifetime(float min_seconds, float max_seconds) {
    RangeParams* p = calloc(1, sizeof(RangeParams));
    if (!p)
        return NULL;
    p->min = min_seconds;
    p->max = max_seconds;
    return module_new("init_lifetime", PARTICLE_PHASE_INIT, init_lifetime_run, p);
}

static void init_size_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                          float t) {
    (void)dt;
    (void)t;
    const RangeParams* p = m->params;
    for (size_t i = begin; i < end; i++)
        e->pool->size[i] = p->min + (p->max - p->min) * particle_emitter_rand01(e);
}

ParticleModule* particle_module_init_size(float min_size, float max_size) {
    RangeParams* p = calloc(1, sizeof(RangeParams));
    if (!p)
        return NULL;
    p->min = min_size;
    p->max = max_size;
    return module_new("init_size", PARTICLE_PHASE_INIT, init_size_run, p);
}

// --- INIT: color ---

typedef struct {
    vec4 base;
    float jitter;
} ColorParams;

static void init_color_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                           float dt, float t) {
    (void)dt;
    (void)t;
    ColorParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        for (int k = 0; k < 3; k++)
            e->pool->color[i][k] =
                p->base[k] + (particle_emitter_rand01(e) * 2.0f - 1.0f) * p->jitter;
        e->pool->color[i][3] = p->base[3];
    }
}

ParticleModule* particle_module_init_color(vec4 base_rgba, float rgb_jitter) {
    ColorParams* p = calloc(1, sizeof(ColorParams));
    if (!p)
        return NULL;
    glm_vec4_copy(base_rgba, p->base);
    p->jitter = rgb_jitter;
    return module_new("init_color", PARTICLE_PHASE_INIT, init_color_run, p);
}

// --- UPDATE: curl-noise turbulence ---

typedef struct {
    float scale;
    float strength;
    float timescale;
} CurlParams;

static void update_curl_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                            float dt, float t) {
    const CurlParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        vec3 c = {0.0f, 0.0f, 0.0f}; // noise_curl3 writes it (out-param)
        noise_curl3(e->pool->position[i][0] * p->scale, e->pool->position[i][1] * p->scale,
                    e->pool->position[i][2] * p->scale + t * p->timescale, c);
        for (int k = 0; k < 3; k++)
            e->pool->velocity[i][k] += c[k] * p->strength * dt;
    }
}

ParticleModule* particle_module_update_curl_noise(float scale, float strength, float timescale) {
    CurlParams* p = calloc(1, sizeof(CurlParams));
    if (!p)
        return NULL;
    p->scale = scale;
    p->strength = strength;
    p->timescale = timescale;
    return module_new("update_curl_noise", PARTICLE_PHASE_UPDATE, update_curl_run, p);
}

void particle_module_curl_set_strength(ParticleModule* m, float strength) {
    if (m && m->params)
        ((CurlParams*)m->params)->strength = strength;
}

// --- UPDATE: drift (constant acceleration) ---

typedef struct {
    vec3 accel;
} DriftParams;

static void update_drift_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                             float dt, float t) {
    (void)t;
    DriftParams* p = m->params;
    for (size_t i = begin; i < end; i++)
        for (int k = 0; k < 3; k++)
            e->pool->velocity[i][k] += p->accel[k] * dt;
}

ParticleModule* particle_module_update_drift(vec3 accel) {
    DriftParams* p = calloc(1, sizeof(DriftParams));
    if (!p)
        return NULL;
    glm_vec3_copy(accel, p->accel);
    return module_new("update_drift", PARTICLE_PHASE_UPDATE, update_drift_run, p);
}

// --- UPDATE: integrate + damping ---

typedef struct {
    float drag;
} IntegrateParams;

static void update_integrate_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                                 float dt, float t) {
    (void)t;
    const IntegrateParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        for (int k = 0; k < 3; k++)
            e->pool->position[i][k] += e->pool->velocity[i][k] * dt;
        for (int k = 0; k < 3; k++)
            e->pool->velocity[i][k] *= p->drag;
    }
}

ParticleModule* particle_module_update_integrate(float drag) {
    IntegrateParams* p = calloc(1, sizeof(IntegrateParams));
    if (!p)
        return NULL;
    p->drag = drag;
    return module_new("update_integrate", PARTICLE_PHASE_UPDATE, update_integrate_run, p);
}
