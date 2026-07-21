#ifndef _PARTICLE_SIM_H_
#define _PARTICLE_SIM_H_

#include <stddef.h>
#include <GL/glew.h>
#include <cglm/cglm.h>

struct ParticleEmitter;

// Canonical per-instance layout: the SINGLE contract shared by the CPU backend
// (which packs it) and the billboard renderer (which binds it as instanced
// vertex attributes). 48 bytes with explicit padding, std430-friendly for a
// future GL 4.3 compute backend. See specs/5.0-particle-system.md.
// (A per-instance velocity for M5 velocity-stretch will re-add a vec4 here.)
typedef struct ParticleInstanceData {
    vec3 center;
    float _pad0; // offset 0
    vec4 params; // offset 16: x=size, y=rotation, z=lifeFrac, w=seed
    vec4 color;  // offset 32: linear HDR rgba (base)
} ParticleInstanceData;

// GPU-resident particle state for the transform-feedback backend. The FIRST 48
// bytes are byte-identical to ParticleInstanceData (center/params/color at the
// same offsets 0/16/32), so the billboard renderer binds instance attributes
// 9/10/11 straight out of this buffer -- only the stride differs (80 vs 48). The
// tail carries the sim-only fields the update shader ping-pongs. Laid out as 5
// vec4 because interleaved transform feedback packs varyings tightly in
// declaration order (no std140 padding), so particle_sim_vert.glsl's out
// varyings MUST match this field order exactly. See
// specs/5.2-particle-gpu-transform-feedback.md.
typedef struct ParticleGpuState {
    vec4 center;  // xyz = world position, w = pad          (offset 0)
    vec4 params;  // x=size, y=rotation, z=lifeFrac, w=seed  (offset 16)
    vec4 color;   // linear HDR rgba (base)                  (offset 32)
    vec4 vel_age; // xyz = velocity, w = age                 (offset 48)
    vec4 life;    // x = lifetime, yzw = free                (offset 64)
} ParticleGpuState;

// What a renderer consumes -- never the pool. The CPU path fills cpu_instances;
// a future GPU backend would fill gpu_instance_vbo with zero readback and the
// renderer's draw path stays byte-identical.
typedef struct ParticleInstanceView {
    size_t count;
    const ParticleInstanceData* cpu_instances; // CPU path (GPU path: NULL)
    GLuint gpu_instance_vbo;                   // GPU path (CPU path: 0)
} ParticleInstanceView;

// The sim backend OWNS the stepping. simulate() runs the emitter's module
// phases (spawn -> init -> update) plus infrastructure (seeding, age advance,
// compaction). acquire_instances() packs the current live set into a view.
// The same vtable admits a future GL 4.3 compute backend.
typedef struct ParticleSimBackend {
    const char* name;
    void (*simulate)(struct ParticleSimBackend* b, struct ParticleEmitter* e, float dt, float t);
    void (*acquire_instances)(struct ParticleSimBackend* b, struct ParticleEmitter* e,
                              ParticleInstanceView* out);
    void (*free_fn)(struct ParticleSimBackend* b);
} ParticleSimBackend;

ParticleSimBackend* create_cpu_particle_sim_backend(void);

// GPU backend: emission + lifecycle stay on the CPU (reusing the emitter's
// spawn/init modules via a ring buffer), the expensive per-particle update runs
// on the GPU via transform feedback. Additive sibling of the CPU backend.
ParticleSimBackend* create_tf_particle_sim_backend(void);

#endif // _PARTICLE_SIM_H_
