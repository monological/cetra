#ifndef _PARTICLE_SYSTEM_H_
#define _PARTICLE_SYSTEM_H_

#include <stddef.h>

#include "particle_emitter.h"
#include "particle_sim.h"
#include "particle_renderer.h"

struct SceneNode; // forward-declared: scene.h holds ParticleSystem*, we hold SceneNode* -- no cycle

// The whole effect: a set of emitters driven by one sim backend. Optionally
// attached to a SceneNode, whose world transform becomes the emitters' spawn
// frame. See specs/5.0-particle-system.md, specs/5.1-particle-scene-integration.md.
typedef struct ParticleSystem {
    char* name;
    ParticleEmitter** emitters;
    size_t emitter_count, emitter_cap;
    ParticleSimBackend* backend; // owned
    struct SceneNode* node;      // borrowed; NULL = world origin
} ParticleSystem;

ParticleSystem* create_particle_system(const char* name);
void free_particle_system(ParticleSystem* s);

// Assign the sim backend (takes ownership; frees any previous one).
void particle_system_set_backend(ParticleSystem* s, ParticleSimBackend* backend);

// Add an emitter (takes ownership).
void particle_system_add_emitter(ParticleSystem* s, ParticleEmitter* e);

// Total live particles across all emitters. Cheap O(emitters); lets the renderer
// skip the scene-depth resolve + draw on frames where nothing is alive.
size_t particle_system_live_count(const ParticleSystem* s);

// Step every emitter through the backend (call from the fixed-timestep update).
void particle_system_update(ParticleSystem* s, float dt, float t);

// Acquire each emitter's instances and draw them. The caller sets up blend /
// depth-mask state around this (particles are transparent).
void particle_system_render(ParticleSystem* s, const ParticleRenderContext* ctx);

#endif // _PARTICLE_SYSTEM_H_
