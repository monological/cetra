#include "particle_sim.h"
#include "particle_emitter.h"
#include "particle_module.h"
#include "program.h"
#include "uniform.h"
#include "util.h"

#include <GL/glew.h>
#include <stdbool.h>
#include <stdlib.h>

// Transform-feedback GPU sim backend (spec 5.2). The CPU still owns emission and
// lifecycle -- it runs the emitter's spawn/init modules (via particle_emitter_*)
// and writes new particles into a ring region of the state buffer -- while the
// expensive per-particle UPDATE (curl noise + integrate) runs on the GPU via a
// transform-feedback pass over two ping-ponged state buffers. Additive sibling of
// the CPU backend.
//
// Ring recycling: with capacity C >= rate * max_lifetime, the emit head only laps
// back onto slots that have already aged out, so no free-list or GPU->CPU
// readback is needed. Dead slots render invisibly (the sim shader forces size 0)
// until re-emitted.

// Per-emitter GPU state, hung off emitter->sim_state (freed via sim_state_free).
typedef struct {
    GLuint vbo[2];  // ping-pong ParticleGpuState buffers
    GLuint vao[2];  // one VAO per vbo, capturing the state input attributes
    int parity;     // vbo[parity] holds the current live state
    size_t head;    // ring emit cursor into the state buffer
    size_t emitted; // high-water count of ever-emitted slots, capped at capacity
    size_t capacity;
    ParticleGpuState* staging; // reused CPU pack buffer for newly emitted slots

    // Update params, read once from the emitter's update modules; the sim shader's
    // constant uniforms are uploaded once too (see setup_done).
    float curl_scale, curl_strength, curl_timescale;
    vec3 drift;
    float drag;
    bool setup_done;
} TfEmitterState;

typedef struct {
    ParticleSimBackend base;
    ShaderProgram* program; // owned; the shared TF update program
} TfSimBackend;

// Point a VAO's 5 state attributes (locations 0..4, one vec4 each) into `vbo`,
// matching the ParticleGpuState layout the sim shader reads.
static void tf_setup_state_vao(GLuint vao, GLuint vbo) {
    const GLsizei stride = (GLsizei)sizeof(ParticleGpuState);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    for (int loc = 0; loc < 5; loc++) {
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, stride, (void*)(loc * sizeof(vec4)));
        glEnableVertexAttribArray(loc);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void tf_state_free(void* p) {
    TfEmitterState* s = p;
    if (!s)
        return;
    glDeleteVertexArrays(2, s->vao);
    glDeleteBuffers(2, s->vbo);
    free(s->staging);
    free(s);
}

static TfEmitterState* tf_ensure_state(ParticleEmitter* e) {
    if (e->sim_state)
        return e->sim_state;

    TfEmitterState* s = calloc(1, sizeof(TfEmitterState));
    if (!s)
        return NULL;
    s->capacity = e->pool ? e->pool->capacity : 0;
    s->staging = malloc(s->capacity * sizeof(ParticleGpuState));
    void* zeros = calloc(s->capacity, sizeof(ParticleGpuState));
    if (!s->staging || !zeros) {
        free(s->staging);
        free(zeros);
        free(s);
        return NULL;
    }

    // Allocate both buffers ZEROED so every slot starts dead (age 0 >= lifetime 0
    // -> the sim shader forces size 0 until the ring emits into the slot).
    glGenBuffers(2, s->vbo);
    glGenVertexArrays(2, s->vao);
    const GLsizeiptr bytes = (GLsizeiptr)(s->capacity * sizeof(ParticleGpuState));
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, s->vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, bytes, zeros, GL_DYNAMIC_COPY);
        tf_setup_state_vao(s->vao[i], s->vbo[i]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    free(zeros);

    e->sim_state = s;
    e->sim_state_free = tf_state_free;
    check_gl_error("particle tf state init");
    return s;
}

// Read the update-phase parameters out of the emitter's module list once. The
// readers no-op on non-matching modules, so calling all three per module is safe.
static void tf_read_params(ParticleEmitter* e, TfEmitterState* s) {
    s->curl_scale = 0.0f;
    s->curl_strength = 0.0f;
    s->curl_timescale = 0.0f;
    glm_vec3_zero(s->drift);
    s->drag = 1.0f;
    for (size_t i = 0; i < e->n_update; i++) {
        const ParticleModule* m = e->update[i];
        particle_module_read_curl(m, &s->curl_scale, &s->curl_strength, &s->curl_timescale);
        particle_module_read_drift(m, s->drift);
        particle_module_read_integrate(m, &s->drag);
    }
}

// CPU emission: init `want` new particles into pool scratch [0,want) (reusing the
// emitter's init modules, honoring local_to_world), pack them, and upload into the
// ring region of the READ buffer so the GPU update pass processes them this frame.
static void tf_emit_ring(ParticleEmitter* e, TfEmitterState* s, int read, size_t want, float dt,
                         float t) {
    ParticlePool* pool = e->pool;
    particle_emitter_init_slice(e, 0, want, dt, t);

    for (size_t j = 0; j < want; j++) {
        ParticleGpuState* d = &s->staging[j];
        glm_vec3_copy(pool->position[j], d->center);
        d->center[3] = 0.0f;
        d->params[0] = pool->size[j];
        d->params[1] = pool->rotation[j];
        d->params[2] = 0.0f; // lifeFrac (fresh)
        d->params[3] = pool->seed[j];
        glm_vec4_copy(pool->color[j], d->color);
        glm_vec3_copy(pool->velocity[j], d->vel_age);
        d->vel_age[3] = 0.0f; // age
        d->life[0] = pool->lifetime[j];
        d->life[1] = 0.0f;
        d->life[2] = 0.0f;
        d->life[3] = 0.0f;
    }

    // Upload into ring slots [head, head+want) mod capacity (up to two runs).
    const size_t cap = s->capacity;
    const size_t head = s->head;
    size_t first = want;
    if (head + first > cap)
        first = cap - head;
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo[read]);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(head * sizeof(ParticleGpuState)),
                    (GLsizeiptr)(first * sizeof(ParticleGpuState)), s->staging);
    if (want > first)
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((want - first) * sizeof(ParticleGpuState)),
                        s->staging + first);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s->head = (head + want) % cap;
}

static void tf_simulate(ParticleSimBackend* b, ParticleEmitter* e, float dt, float t) {
    TfSimBackend* self = (TfSimBackend*)b;
    if (!self->program)
        return;
    TfEmitterState* s = tf_ensure_state(e);
    if (!s || s->capacity == 0)
        return;
    if (!s->setup_done)
        tf_read_params(e, s);

    const int read = s->parity;
    const int write = read ^ 1;

    // 1. Emission (CPU): spawn modules -> want, init + upload into the READ buffer.
    size_t want = particle_emitter_run_spawn(e, dt, t);
    if (want > 0) {
        tf_emit_ring(e, s, read, want, dt, t);
        s->emitted += want;
        if (s->emitted > s->capacity)
            s->emitted = s->capacity;
    }

    // 2. Update (GPU): read[read] -> write[write] via transform feedback over the
    //    active prefix [0, emitted), no rasterization. Fully bracketed so no state
    //    leaks into the render passes.
    glEnable(GL_RASTERIZER_DISCARD);
    glUseProgram(self->program->id);
    UniformManager* u = self->program->uniforms;
    if (!s->setup_done) {
        // The curl/drift/drag params never change, so upload them once (v1 is a
        // single emitter per shared sim program; multi-emitter would re-upload).
        uniform_set_float(u, "curlScale", s->curl_scale);
        uniform_set_float(u, "curlStrength", s->curl_strength);
        uniform_set_float(u, "curlTimescale", s->curl_timescale);
        uniform_set_vec3(u, "drift", s->drift);
        uniform_set_float(u, "drag", s->drag);
        s->setup_done = true;
    }
    uniform_set_float(u, "dt", dt);
    uniform_set_float(u, "time", t);

    glBindVertexArray(s->vao[read]);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, s->vbo[write]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, (GLsizei)s->emitted);
    glEndTransformFeedback();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_RASTERIZER_DISCARD);

    s->parity = write;
    check_gl_error("particle tf simulate");
}

static void tf_acquire_instances(ParticleSimBackend* b, ParticleEmitter* e,
                                 ParticleInstanceView* out) {
    (void)b;
    TfEmitterState* s = e->sim_state;
    if (!s) {
        out->count = 0;
        out->cpu_instances = NULL;
        out->gpu_instance_vbo = 0;
        return;
    }
    // Draw the active prefix [0, emitted) (dead slots inside it carry size 0). The
    // renderer binds instance attrs straight from this VBO -- zero readback.
    out->count = s->emitted;
    out->cpu_instances = NULL;
    out->gpu_instance_vbo = s->vbo[s->parity];
}

static size_t tf_live_count(ParticleSimBackend* b, ParticleEmitter* e) {
    (void)b;
    TfEmitterState* s = e->sim_state;
    return s ? s->emitted : 0;
}

static void tf_backend_free(ParticleSimBackend* b) {
    TfSimBackend* self = (TfSimBackend*)b;
    if (self->program)
        free_program(self->program);
    free(self);
}

ParticleSimBackend* create_tf_particle_sim_backend(void) {
    TfSimBackend* self = calloc(1, sizeof(TfSimBackend));
    if (!self)
        return NULL;
    self->base.name = "tf";
    self->base.simulate = tf_simulate;
    self->base.acquire_instances = tf_acquire_instances;
    self->base.live_count = tf_live_count;
    self->base.free_fn = tf_backend_free;
    self->program = create_particle_sim_program();
    if (!self->program) {
        free(self);
        return NULL;
    }
    return &self->base;
}
