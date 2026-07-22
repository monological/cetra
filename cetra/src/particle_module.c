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
    if (!m || m->phase != PARTICLE_PHASE_SPAWN || !m->params)
        return;
    ((SpawnRateParams*)m->params)->rate = rate;
}

// --- INIT: box location ---
// NB: location/position init modules must place particles through the emitter's
// local_to_world (below), so the node transform is honored. A new location
// module that writes pool->position directly would silently ignore the node.

typedef struct {
    vec3 min, max;
} BoxLocParams;

static void init_box_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                         float t) {
    (void)dt;
    (void)t;
    const BoxLocParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        // Sample the box in the emitter's LOCAL frame, then place it in the
        // world via the emitter's spawn transform (identity if unattached).
        vec3 local = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 3; k++)
            local[k] = p->min[k] + (p->max[k] - p->min[k]) * particle_emitter_rand01(e);
        glm_mat4_mulv3(e->local_to_world, local, 1.0f, e->pool->position[i]);
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
        glm_vec3_muladds(c, p->strength * dt, e->pool->velocity[i]);
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

// --- UPDATE: drift (constant acceleration) ---

typedef struct {
    vec3 accel;
} DriftParams;

static void update_drift_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                             float dt, float t) {
    (void)t;
    const DriftParams* p = m->params;
    for (size_t i = begin; i < end; i++)
        glm_vec3_muladds(p->accel, dt, e->pool->velocity[i]);
}

ParticleModule* particle_module_update_drift(vec3 accel) {
    DriftParams* p = calloc(1, sizeof(DriftParams));
    if (!p)
        return NULL;
    glm_vec3_copy(accel, p->accel);
    return module_new("update_drift", PARTICLE_PHASE_UPDATE, update_drift_run, p);
}

// --- UPDATE: rotation (billboard roll) ---
// Spin rate is derived from the particle's own seed rather than stored, so it
// stays constant over the particle's life without widening the pool. The sign
// alternates by seed so a drift of falling leaves tumbles both ways.

typedef struct {
    float min_rate;
    float max_rate;
} RotationParams;

static void update_rotation_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                                float dt, float t) {
    (void)t;
    const RotationParams* p = m->params;
    for (size_t i = begin; i < end; i++) {
        float s = e->pool->seed[i];
        float frac = s * 7.31f;
        frac -= floorf(frac); // decorrelate from whatever else reads the seed
        float rate = p->min_rate + (p->max_rate - p->min_rate) * frac;
        e->pool->rotation[i] += (s < 0.5f ? -rate : rate) * dt;
    }
}

ParticleModule* particle_module_update_rotation(float min_rate, float max_rate) {
    RotationParams* p = calloc(1, sizeof(RotationParams));
    if (!p)
        return NULL;
    p->min_rate = min_rate;
    p->max_rate = max_rate;
    return module_new("update_rotation", PARTICLE_PHASE_UPDATE, update_rotation_run, p);
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
        glm_vec3_muladds(e->pool->velocity[i], dt, e->pool->position[i]);
        glm_vec3_scale(e->pool->velocity[i], p->drag, e->pool->velocity[i]);
    }
}

ParticleModule* particle_module_update_integrate(float drag) {
    IntegrateParams* p = calloc(1, sizeof(IntegrateParams));
    if (!p)
        return NULL;
    p->drag = drag;
    return module_new("update_integrate", PARTICLE_PHASE_UPDATE, update_integrate_run, p);
}

// --- UPDATE: analytic collider (one primitive; shape + mode are data) ---
// Repels particles out of (KEEP_OUT) or clamps them inside (KEEP_IN) an analytic
// shape. Runs AFTER update_integrate so it corrects the just-moved position. A
// collider moved via particle_module_collider_set imparts its own velocity as a
// wake (KEEP_OUT), so a moving obstacle shoves dust along. Add a shape by adding
// a ColliderShape case + a resolve branch here (and the matching shader branch).

typedef struct {
    int shape; // ColliderShape
    int mode;  // ColliderMode
    vec3 a;    // sphere: center;  box: min
    vec3 b;    // sphere: unused;  box: max
    vec3 prev_a; // last step's `a`; (a - prev_a)/dt is the shape velocity (wake)
    float radius;
    float restitution;
    float wake;
} ColliderParams;

// Closest point on segment [s0,s1] to `pt`, written to `out`.
static void closest_on_segment(const float* s0, const float* s1, const float* pt, float* out) {
    vec3 ab, ap;
    glm_vec3_sub((float*)s1, (float*)s0, ab);
    glm_vec3_sub((float*)pt, (float*)s0, ap);
    float denom = glm_vec3_dot(ab, ab);
    float u = denom > 1e-8f ? glm_vec3_dot(ap, ab) / denom : 0.0f;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    glm_vec3_copy((float*)s0, out);
    glm_vec3_muladds(ab, u, out);
}

// Keep a particle on the correct side of a sphere of radius `p->radius` centered
// at `c`: snap to the surface and reflect the into-surface velocity by
// (1 + restitution). Shared by the sphere, capsule, and cylinder-side cases.
static void resolve_radial(const ColliderParams* p, const float* c, const float* svel, float* pos,
                           float* vel) {
    vec3 d;
    glm_vec3_sub(pos, (float*)c, d);
    float dist = glm_vec3_norm(d);
    bool wrong_side = (p->mode == COLLIDER_KEEP_OUT) ? (dist < p->radius) : (dist > p->radius);
    if (!wrong_side)
        return;
    vec3 n; // radial outward normal (guard the exact-center degenerate case)
    if (dist > 1e-5f)
        glm_vec3_scale(d, 1.0f / dist, n);
    else
        glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, n);
    glm_vec3_copy((float*)c, pos); // snap onto the surface
    glm_vec3_muladds(n, p->radius, pos);
    // Into-surface component: inward (vn<0) for keep-out, outward (vn>0) for keep-in.
    float vn = glm_vec3_dot(vel, n);
    bool into = (p->mode == COLLIDER_KEEP_OUT) ? (vn < 0.0f) : (vn > 0.0f);
    if (into)
        glm_vec3_muladds(n, -vn * (1.0f + p->restitution), vel);
    if (p->mode == COLLIDER_KEEP_OUT) // wake only makes sense for an obstacle
        glm_vec3_muladds((float*)svel, p->wake, vel);
}

// Plane through `a` with unit normal `b`: keep the particle on the +normal side.
static void collider_resolve_plane(const ColliderParams* p, const float* svel, float* pos,
                                   float* vel) {
    vec3 rel;
    glm_vec3_sub(pos, (float*)p->a, rel);
    float sd = glm_vec3_dot(rel, (float*)p->b); // signed distance along the normal
    if (sd >= 0.0f)
        return;
    glm_vec3_muladds((float*)p->b, -sd, pos); // lift onto the plane
    float vn = glm_vec3_dot(vel, (float*)p->b);
    if (vn < 0.0f)
        glm_vec3_muladds((float*)p->b, -vn * (1.0f + p->restitution), vel);
    glm_vec3_muladds((float*)svel, p->wake, vel); // plane is always keep-out
}

// Finite cylinder: axis [a,b], radius `radius`, flat end caps. Ejects a
// penetrating particle (KEEP_OUT) through the least-penetrated of the curved
// side / the two caps, or clamps it inside (KEEP_IN).
static void collider_resolve_cylinder(const ColliderParams* p, const float* svel, float* pos,
                                      float* vel) {
    vec3 axis;
    glm_vec3_sub((float*)p->b, (float*)p->a, axis);
    float axlen = glm_vec3_norm(axis);
    if (axlen < 1e-6f)
        return;
    glm_vec3_scale(axis, 1.0f / axlen, axis); // unit axis
    vec3 rel;
    glm_vec3_sub(pos, (float*)p->a, rel);
    float h = glm_vec3_dot(rel, axis); // axial coord from a, in [0,axlen] inside
    vec3 foot;                         // projection onto the infinite axis line
    glm_vec3_copy((float*)p->a, foot);
    glm_vec3_muladds(axis, h, foot);
    vec3 radial;
    glm_vec3_sub(pos, foot, radial);
    float rdist = glm_vec3_norm(radial);
    vec3 rn; // radial outward normal
    if (rdist > 1e-5f)
        glm_vec3_scale(radial, 1.0f / rdist, rn);
    else
        glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, rn);

    if (p->mode == COLLIDER_KEEP_OUT) {
        if (rdist >= p->radius || h <= 0.0f || h >= axlen)
            return; // outside the finite cylinder
        float side_pen = p->radius - rdist;
        float cap_pen = h < axlen - h ? h : axlen - h;
        if (side_pen <= cap_pen) { // eject through the curved side
            glm_vec3_copy(foot, pos);
            glm_vec3_muladds(rn, p->radius, pos);
            float vn = glm_vec3_dot(vel, rn);
            if (vn < 0.0f)
                glm_vec3_muladds(rn, -vn * (1.0f + p->restitution), vel);
        } else { // eject through the nearer cap (foot + radial == pos, so move axially only)
            float cap_h = h < axlen - h ? 0.0f : axlen;
            glm_vec3_muladds(axis, cap_h - h, pos);
            float va = glm_vec3_dot(vel, axis);
            bool into = (h < axlen - h) ? (va > 0.0f) : (va < 0.0f);
            if (into)
                glm_vec3_muladds(axis, -va * (1.0f + p->restitution), vel);
        }
        glm_vec3_muladds((float*)svel, p->wake, vel);
        return;
    }
    // KEEP_IN: clamp radially into the tube and axially between the caps.
    if (rdist > p->radius) {
        glm_vec3_copy(foot, pos);
        glm_vec3_muladds(rn, p->radius, pos);
        float vn = glm_vec3_dot(vel, rn);
        if (vn > 0.0f)
            glm_vec3_muladds(rn, -vn * (1.0f + p->restitution), vel);
    }
    if (h < 0.0f || h > axlen) {
        float cap_h = h < 0.0f ? 0.0f : axlen;
        glm_vec3_muladds(axis, cap_h - h, pos);
        float vn = glm_vec3_dot(vel, axis);
        if ((h < 0.0f && vn < 0.0f) || (h > axlen && vn > 0.0f))
            glm_vec3_muladds(axis, -vn * (1.0f + p->restitution), vel);
    }
}

// Resolve a particle against an AABB [a,b].
static void collider_resolve_box(const ColliderParams* p, float* pos, float* vel) {
    if (p->mode == COLLIDER_KEEP_IN) {
        for (int k = 0; k < 3; k++) {
            if (pos[k] < p->a[k]) {
                pos[k] = p->a[k];
                if (vel[k] < 0.0f)
                    vel[k] *= -p->restitution;
            } else if (pos[k] > p->b[k]) {
                pos[k] = p->b[k];
                if (vel[k] > 0.0f)
                    vel[k] *= -p->restitution;
            }
        }
        return;
    }
    // KEEP_OUT: act only when inside; eject through the least-penetrated face.
    if (pos[0] <= p->a[0] || pos[0] >= p->b[0] || pos[1] <= p->a[1] || pos[1] >= p->b[1] ||
        pos[2] <= p->a[2] || pos[2] >= p->b[2])
        return;
    int best_k = 0, best_sign = -1;
    float best_pen = pos[0] - p->a[0], best_face = p->a[0];
    for (int k = 0; k < 3; k++) {
        float lo = pos[k] - p->a[k], hi = p->b[k] - pos[k];
        if (lo < best_pen) {
            best_pen = lo;
            best_k = k;
            best_face = p->a[k];
            best_sign = -1;
        }
        if (hi < best_pen) {
            best_pen = hi;
            best_k = k;
            best_face = p->b[k];
            best_sign = 1;
        }
    }
    pos[best_k] = best_face;
    if ((best_sign < 0 && vel[best_k] < 0.0f) || (best_sign > 0 && vel[best_k] > 0.0f))
        vel[best_k] *= -p->restitution;
}

// The collider's own translation velocity this step (drives the wake). Shared by
// collider_run (CPU) and particle_module_read_collider (packed for the GPU).
static void collider_shape_velocity(const ColliderParams* p, float dt, float* out) {
    if (dt > 0.0f) {
        glm_vec3_sub((float*)p->a, (float*)p->prev_a, out);
        glm_vec3_scale(out, 1.0f / dt, out);
    } else {
        glm_vec3_zero(out);
    }
}

static void collider_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                         float t) {
    (void)t;
    const ColliderParams* p = m->params;
    vec3 svel = {0.0f, 0.0f, 0.0f}; // out-param of collider_shape_velocity
    collider_shape_velocity(p, dt, svel);
    for (size_t i = begin; i < end; i++) {
        float* pos = e->pool->position[i];
        float* vel = e->pool->velocity[i];
        switch (p->shape) {
        case COLLIDER_SPHERE:
            resolve_radial(p, p->a, svel, pos, vel);
            break;
        case COLLIDER_BOX:
            collider_resolve_box(p, pos, vel);
            break;
        case COLLIDER_PLANE:
            collider_resolve_plane(p, svel, pos, vel);
            break;
        case COLLIDER_CAPSULE: {
            vec3 c = {0.0f, 0.0f, 0.0f}; // out-param of closest_on_segment
            closest_on_segment(p->a, p->b, pos, c);
            resolve_radial(p, c, svel, pos, vel);
            break;
        }
        case COLLIDER_CYLINDER:
            collider_resolve_cylinder(p, svel, pos, vel);
            break;
        }
    }
}

static ParticleModule* collider_new(const ColliderParams* init) {
    ColliderParams* p = calloc(1, sizeof(ColliderParams));
    if (!p)
        return NULL;
    *p = *init;
    glm_vec3_copy((float*)init->a, p->prev_a);
    return module_new("collider", PARTICLE_PHASE_UPDATE, collider_run, p);
}

ParticleModule* particle_module_collider_sphere(vec3 center, float radius, ColliderMode mode,
                                                float restitution, float wake) {
    ColliderParams init = {0};
    init.shape = COLLIDER_SPHERE;
    init.mode = mode;
    glm_vec3_copy(center, init.a);
    init.radius = radius;
    init.restitution = restitution;
    init.wake = wake;
    return collider_new(&init);
}

ParticleModule* particle_module_collider_box(vec3 min, vec3 max, ColliderMode mode,
                                             float restitution) {
    ColliderParams init = {0};
    init.shape = COLLIDER_BOX;
    init.mode = mode;
    glm_vec3_copy(min, init.a);
    glm_vec3_copy(max, init.b);
    init.restitution = restitution;
    return collider_new(&init);
}

ParticleModule* particle_module_collider_plane(vec3 point, vec3 normal, float restitution) {
    ColliderParams init = {0};
    init.shape = COLLIDER_PLANE;
    init.mode = COLLIDER_KEEP_OUT; // a half-space; the normal picks the kept side
    glm_vec3_copy(point, init.a);
    glm_vec3_normalize_to(normal, init.b);
    init.restitution = restitution;
    return collider_new(&init);
}

ParticleModule* particle_module_collider_capsule(vec3 p0, vec3 p1, float radius, ColliderMode mode,
                                                 float restitution, float wake) {
    ColliderParams init = {0};
    init.shape = COLLIDER_CAPSULE;
    init.mode = mode;
    glm_vec3_copy(p0, init.a);
    glm_vec3_copy(p1, init.b);
    init.radius = radius;
    init.restitution = restitution;
    init.wake = wake;
    return collider_new(&init);
}

ParticleModule* particle_module_collider_cylinder(vec3 p0, vec3 p1, float radius, ColliderMode mode,
                                                  float restitution, float wake) {
    ColliderParams init = {0};
    init.shape = COLLIDER_CYLINDER;
    init.mode = mode;
    glm_vec3_copy(p0, init.a);
    glm_vec3_copy(p1, init.b);
    init.radius = radius;
    init.restitution = restitution;
    init.wake = wake;
    return collider_new(&init);
}

void particle_module_collider_set(ParticleModule* m, vec3 a, vec3 b, float radius) {
    if (!m || m->run != collider_run)
        return;
    ColliderParams* p = m->params;
    glm_vec3_copy(p->a, p->prev_a); // last step's placement -> prev (derives the wake)
    glm_vec3_copy(a, p->a);
    glm_vec3_copy(b, p->b);
    p->radius = radius;
}

// --- GPU-backend introspection (see particle_module.h) ---
// Matched on the run-fn pointer (compiler-checked) rather than the name string,
// so a renamed label can't silently break the GPU translation.

bool particle_module_read_curl(const ParticleModule* m, float* scale, float* strength,
                               float* timescale) {
    if (!m || m->run != update_curl_run)
        return false;
    const CurlParams* p = m->params;
    if (scale)
        *scale = p->scale;
    if (strength)
        *strength = p->strength;
    if (timescale)
        *timescale = p->timescale;
    return true;
}

bool particle_module_read_drift(const ParticleModule* m, vec3 accel_out) {
    if (!m || m->run != update_drift_run)
        return false;
    const DriftParams* p = m->params;
    accel_out[0] = p->accel[0];
    accel_out[1] = p->accel[1];
    accel_out[2] = p->accel[2];
    return true;
}

bool particle_module_read_integrate(const ParticleModule* m, float* drag) {
    if (!m || m->run != update_integrate_run)
        return false;
    const IntegrateParams* p = m->params;
    if (drag)
        *drag = p->drag;
    return true;
}

bool particle_module_read_collider(const ParticleModule* m, ColliderState* out, float dt) {
    if (!m || m->run != collider_run)
        return false;
    const ColliderParams* p = m->params;
    if (out) {
        out->shape = p->shape;
        out->mode = p->mode;
        glm_vec3_copy((float*)p->a, out->a);
        glm_vec3_copy((float*)p->b, out->b);
        collider_shape_velocity(p, dt, out->vel);
        out->radius = p->radius;
        out->restitution = p->restitution;
        out->wake = p->wake;
    }
    return true;
}
