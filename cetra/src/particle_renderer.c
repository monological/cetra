#include "particle_renderer.h"
#include "uniform.h"

#include <stddef.h>
#include <stdlib.h>

// Billboard renderer: one static unit-quad, one dynamic per-instance buffer,
// drawn with glDrawArraysInstanced. This is the engine's first instanced-draw
// path (setup mirrors the bone-line VAO, upload mirrors its per-frame reupload).
typedef struct {
    ShaderProgram* program; // borrowed (engine-owned)
    GLuint vao;
    GLuint quad_vbo;
    GLuint instance_vbo;
    size_t upload_count; // instances uploaded in prepare(), drawn in draw()
} BillboardRenderer;

// Unit quad corners in [-1,1], two triangles. The corner doubles as the radial
// coordinate for the soft sprite in the fragment shader.
static const float k_quad[12] = {
    -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
};

static void billboard_setup(BillboardRenderer* b) {
    glGenVertexArrays(1, &b->vao);
    glGenBuffers(1, &b->quad_vbo);
    glGenBuffers(1, &b->instance_vbo);

    glBindVertexArray(b->vao);

    // Static unit quad: slot 0 = corner (vec2).
    glBindBuffer(GL_ARRAY_BUFFER, b->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_quad), k_quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Per-instance ParticleInstanceData on free attribute slots (>=9).
    const GLsizei stride = (GLsizei)sizeof(ParticleInstanceData);
    glBindBuffer(GL_ARRAY_BUFFER, b->instance_vbo);
    glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, center));
    glEnableVertexAttribArray(9);
    glVertexAttribDivisor(9, 1);
    glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, params));
    glEnableVertexAttribArray(10);
    glVertexAttribDivisor(10, 1);
    glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, color));
    glEnableVertexAttribArray(11);
    glVertexAttribDivisor(11, 1);
    glVertexAttribPointer(12, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, velocity));
    glEnableVertexAttribArray(12);
    glVertexAttribDivisor(12, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void billboard_prepare(ParticleRenderer* r, const ParticleInstanceView* view,
                              const ParticleRenderContext* ctx) {
    (void)ctx;
    BillboardRenderer* b = r->impl;
    b->upload_count = 0;
    if (!view || view->count == 0 || !view->cpu_instances)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, b->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(view->count * sizeof(ParticleInstanceData)),
                 view->cpu_instances, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    b->upload_count = view->count;
}

static void billboard_draw(ParticleRenderer* r, const ParticleInstanceView* view,
                           const ParticleRenderContext* ctx) {
    (void)view;
    BillboardRenderer* b = r->impl;
    if (b->upload_count == 0 || !b->program)
        return;

    glUseProgram(b->program->id);
    UniformManager* u = b->program->uniforms;
    uniform_set_mat4(u, "view", (const float*)ctx->view);
    uniform_set_mat4(u, "projection", (const float*)ctx->proj);
    uniform_set_float(u, "hdrGain", 6.0f);

    glBindVertexArray(b->vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)b->upload_count);
    glBindVertexArray(0);
}

static void billboard_free(ParticleRenderer* r) {
    if (!r)
        return;
    BillboardRenderer* b = r->impl;
    if (b) {
        glDeleteVertexArrays(1, &b->vao);
        glDeleteBuffers(1, &b->quad_vbo);
        glDeleteBuffers(1, &b->instance_vbo);
        free(b);
    }
    free(r);
}

ParticleRenderer* create_billboard_particle_renderer(ShaderProgram* program) {
    ParticleRenderer* r = calloc(1, sizeof(ParticleRenderer));
    BillboardRenderer* b = calloc(1, sizeof(BillboardRenderer));
    if (!r || !b) {
        free(r);
        free(b);
        return NULL;
    }
    b->program = program;
    billboard_setup(b);

    r->name = "billboard";
    r->prepare = billboard_prepare;
    r->draw = billboard_draw;
    r->free_fn = billboard_free;
    r->impl = b;
    return r;
}
