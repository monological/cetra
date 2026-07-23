#include "particle_renderer.h"
#include "uniform.h"
#include "scene.h"
#include "shadow.h"
#include "light.h"

#include <stddef.h>
#include <stdlib.h>

// Billboard look tunables. Fixed for the one spore emitter today; promote to
// per-emitter renderer config if a second emitter needs a different look.
#define PARTICLE_HDR_GAIN       6.0f  // push color >1.0 so bloom haloes the motes
#define PARTICLE_AMBIENT_FLOOR  0.18f // brightness of motes in full shadow
#define PARTICLE_SOFT_FADE_DIST 0.5f  // world-space soft-particle fade band
#define PARTICLE_DEPTH_UNIT     7     // free sampler unit for the resolved scene depth
#define PARTICLE_SPRITE_UNIT    8     // free sampler unit for the optional sprite

// Billboard renderer: one static unit-quad, one dynamic per-instance buffer,
// drawn with glDrawArraysInstanced. This is the engine's first instanced-draw
// path (setup mirrors the bone-line VAO, upload mirrors its per-frame reupload).
typedef struct {
    ShaderProgram* program; // borrowed (engine-owned)
    GLuint vao;
    GLuint quad_vbo;
    GLuint instance_vbo;
    size_t upload_count; // instances uploaded in prepare(), drawn in draw()
    GLuint sprite_tex;   // 0 = draw the built-in procedural disc
    float hdr_gain;      // defaults to PARTICLE_HDR_GAIN
} BillboardRenderer;

// Unit quad corners in [-1,1], two triangles. The corner doubles as the radial
// coordinate for the soft sprite in the fragment shader.
static const float k_quad[12] = {
    -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
};

// Point slots 9/10/11 (center/params/color) at `buffer` with `stride`.
// center/params/color sit at the same offsets in ParticleInstanceData (CPU path,
// stride 48) and ParticleGpuState (GPU path, stride 80), so only buffer + stride
// vary. Enable + divisor are VAO-persistent (set once by ..._setup_instance_attribs),
// so re-pointing is all the GPU path needs each frame as its buffer ping-pongs.
// The caller must have the target VAO bound.
static void billboard_point_instance_attribs(GLuint buffer, GLsizei stride) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, center));
    glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, params));
    glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, stride,
                          (void*)offsetof(ParticleInstanceData, color));
}

// One-time VAO setup: point + enable + per-instance divisor for slots 9/10/11.
static void billboard_setup_instance_attribs(GLuint buffer, GLsizei stride) {
    billboard_point_instance_attribs(buffer, stride);
    glEnableVertexAttribArray(9);
    glVertexAttribDivisor(9, 1);
    glEnableVertexAttribArray(10);
    glVertexAttribDivisor(10, 1);
    glEnableVertexAttribArray(11);
    glVertexAttribDivisor(11, 1);
}

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

    // Per-instance attributes (slots 9/10/11) bound to our own instance_vbo for
    // the CPU path; the GPU path re-points them at the sim buffer in prepare().
    billboard_setup_instance_attribs(b->instance_vbo, (GLsizei)sizeof(ParticleInstanceData));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void billboard_prepare(ParticleRenderer* r, const ParticleInstanceView* view,
                              const ParticleRenderContext* ctx) {
    (void)ctx;
    BillboardRenderer* b = r->impl;
    b->upload_count = 0;
    if (!view || view->count == 0)
        return;

    if (view->gpu_instance_vbo != 0) {
        // GPU path: the sim backend already wrote this buffer on-GPU (zero
        // readback). Re-point the VAO's instance attributes at it -- the buffer
        // ping-pongs each frame, so this is re-issued per frame -- with the
        // ParticleGpuState stride. No upload.
        glBindVertexArray(b->vao);
        billboard_point_instance_attribs(view->gpu_instance_vbo, (GLsizei)sizeof(ParticleGpuState));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        b->upload_count = view->count;
        return;
    }

    if (!view->cpu_instances)
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
    uniform_set_float(u, "hdrGain", b->hdr_gain);

    // Optional sprite: replaces the procedural disc with a texture (leaves,
    // embers, anything with a shape). The quad corner doubles as the UV.
    if (b->sprite_tex) {
        glActiveTexture(GL_TEXTURE0 + PARTICLE_SPRITE_UNIT);
        glBindTexture(GL_TEXTURE_2D, b->sprite_tex);
        uniform_set_int(u, "uSpriteTex", PARTICLE_SPRITE_UNIT);
        uniform_set_int(u, "uSpriteEnabled", 1);
    } else {
        uniform_set_int(u, "uSpriteEnabled", 0);
    }

    // Directional key + CSM shadow (M3): the motes brighten in the light and
    // fall to the ambient floor in shadow. bind_shadow_maps_to_program uploads
    // the whole cascade uniform block + binds the shadow array (unit 10); it is
    // location-guarded, so it only touches uniforms the particle shader declares.
    uniform_set_float(u, "uAmbient", PARTICLE_AMBIENT_FLOOR);
    if (ctx->scene) {
        const Light* sun = NULL;
        for (size_t i = 0; i < ctx->scene->light_count; i++) {
            if (ctx->scene->lights[i] && ctx->scene->lights[i]->type == LIGHT_DIRECTIONAL) {
                sun = ctx->scene->lights[i];
                break;
            }
        }
        if (sun)
            uniform_set_vec3(u, "uSunColor", sun->color);
        if (ctx->scene->shadow_system)
            bind_shadow_maps_to_program(ctx->scene->shadow_system, b->program);
    }

    // Soft particles (M4): bind the resolved scene depth on a free unit and let
    // the shader fade motes as they approach the surface behind them.
    if (ctx->scene_depth_texture) {
        glActiveTexture(GL_TEXTURE0 + PARTICLE_DEPTH_UNIT);
        glBindTexture(GL_TEXTURE_2D, ctx->scene_depth_texture);
        uniform_set_int(u, "sceneDepth", PARTICLE_DEPTH_UNIT);
        uniform_set_int(u, "uSoftEnabled", 1);
        uniform_set_float(u, "softDist", PARTICLE_SOFT_FADE_DIST);
    } else {
        uniform_set_int(u, "uSoftEnabled", 0);
    }

    glBindVertexArray(b->vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)b->upload_count);
    glBindVertexArray(0);

    // Leave the active unit at 0 -- bind_shadow_maps_to_program / the depth bind
    // above leave it at 10 / 7 otherwise, a state leak out of the pass.
    glActiveTexture(GL_TEXTURE0);
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
    b->sprite_tex = 0;
    b->hdr_gain = PARTICLE_HDR_GAIN;
    billboard_setup(b);

    r->name = "billboard";
    r->prepare = billboard_prepare;
    r->draw = billboard_draw;
    r->free_fn = billboard_free;
    r->impl = b;
    return r;
}

void billboard_renderer_set_sprite(ParticleRenderer* r, Texture* tex, float hdr_gain) {
    if (!r || r->prepare != billboard_prepare || !r->impl)
        return;
    BillboardRenderer* b = r->impl;
    b->sprite_tex = tex ? tex->id : 0;
    b->hdr_gain = hdr_gain;
}
