#ifndef _PARTICLE_RENDERER_H_
#define _PARTICLE_RENDERER_H_

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "program.h"
#include "particle_sim.h"
#include "texture.h"

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
//
// What a billboard is, before tuning one: a particle's size is a HALF-width
// (the quad spans size on either side of the centre); its alpha fades in over
// the first tenth of its life and out over the last three tenths; and its
// colour is LIT by default -- brightened toward the scene's key directional
// and dropped to an ambient floor in shadow, with a tint of 0.4 toward the
// key's colour, so a scene with no key light shows every mote at 0.6 of its
// authored colour. billboard_renderer_set_lit(r, false) is the way out of that
// for a sketch whose colours are the picture.
ParticleRenderer* create_billboard_particle_renderer(ShaderProgram* program);

// Draw `tex` on each billboard instead of the built-in procedural soft disc,
// with the quad corner as the UV -- the per-particle roll already spins it, so
// a leaf sprite tumbles for free. `hdr_gain` replaces the default mote gain of
// 6.0 (pass 1.0 for an albedo sprite that should not glow). Passing NULL
// restores the disc. No-op on a renderer that is not a billboard.
void billboard_renderer_set_sprite(ParticleRenderer* r, Texture* tex, float hdr_gain);

// The floor a mote falls to in full shadow (0..1, default 0.18) and the
// world-space band over which it fades into the surface behind it (default
// 0.5). No-op on a renderer that is not a billboard.
void billboard_renderer_set_lighting(ParticleRenderer* r, float ambient_floor,
                                     float soft_fade_dist);

// false = the authored colour times hdr_gain reaches the framebuffer untinted:
// no key-light lookup and no ambient floor. Default true. No-op on a renderer
// that is not a billboard.
void billboard_renderer_set_lit(ParticleRenderer* r, bool lit);

#endif // _PARTICLE_RENDERER_H_
