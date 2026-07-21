#ifndef _PARTICLE_RENDERER_H_
#define _PARTICLE_RENDERER_H_

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "program.h"
#include "particle_sim.h"

struct Scene;

// Everything a renderer needs from the frame -- GL + Scene only, never Engine.
// The host (which has the engine) fills this in. See specs/5.0-particle-system.md.
typedef struct ParticleRenderContext {
    mat4 view;
    mat4 proj;
    struct Scene* scene;        // lights[] + shadow_system (used from M3)
    GLuint scene_depth_texture; // single-sample scene depth for soft particles (0 = off, M4)
} ParticleRenderContext;

// Pluggable output. Billboard now; mesh/ribbon renderers later reuse this
// vtable. A renderer consumes only a ParticleInstanceView, never the pool.
typedef struct ParticleRenderer {
    const char* name;
    void (*prepare)(struct ParticleRenderer* r, const ParticleInstanceView* view,
                    const ParticleRenderContext* ctx); // upload
    void (*draw)(struct ParticleRenderer* r, const ParticleInstanceView* view,
                 const ParticleRenderContext* ctx);
    void (*free_fn)(struct ParticleRenderer* r);
    void* impl;
} ParticleRenderer;

// Instanced camera-facing billboards. `program` is borrowed (engine-owned).
ParticleRenderer* create_billboard_particle_renderer(ShaderProgram* program);

#endif // _PARTICLE_RENDERER_H_
