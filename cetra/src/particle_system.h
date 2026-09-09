#ifndef _PARTICLE_SYSTEM_H_
#define _PARTICLE_SYSTEM_H_

/*
 * A particle system: emitters, each a pool with composable spawn, init and
 * update modules and a pluggable renderer, driven by one sim backend (CPU, or
 * transform feedback on the GPU). A scene citizen: the scene owns it, a node
 * borrows it and lends its world transform as the spawn frame, and the frame
 * ticks and draws it with nothing further from the app (specs 5.0, 5.1).
 *
 * The path from nothing to motes on screen: create the system, set a
 * backend, build an emitter with a renderer and its modules, add the emitter,
 * scene_add_particle_system, then node_set_particle_system on a node under
 * the root. The tick has two homes and only one runs: the game framework's
 * fixed step when the app uses it, the engine's own per-frame hook when it
 * does not.
 */

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

// Subtract a world-origin shift from every position this system holds
// (spec 11.62): the spawn frame, each pool, and whatever the backend keeps of
// its own. Particles are spawned into WORLD space and integrated there, so a
// live population does not follow its emitter's node the way a mesh follows its
// parent -- it stays at the old origin until every particle has aged out.
void particle_system_shift_origin(ParticleSystem* s, const vec3 delta);

// Acquire each emitter's instances and draw them. The caller sets up blend /
// depth-mask state around this (particles are transparent).
void particle_system_render(ParticleSystem* s, const ParticleRenderContext* ctx);

#endif // _PARTICLE_SYSTEM_H_
