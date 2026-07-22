#ifndef _PARTICLE_MODULE_H_
#define _PARTICLE_MODULE_H_

#include <stddef.h>
#include <stdbool.h>
#include <cglm/cglm.h>

// A ParticleModule is one composable behavior in an emitter's stack. Modules
// are fn-pointer descriptors with module-owned opaque params, so a new module
// lives entirely in its own translation unit and needs zero changes to the
// emitter/backend core (the Niagara model). See specs/5.0-particle-system.md.
//
// The [begin,end) slice each kernel receives is deliberately shaped like a GPU
// compute invocation range, so the module boundary survives a future CPU->GPU
// backend swap.

struct ParticleEmitter;
struct ParticleModule;

typedef enum {
    PARTICLE_PHASE_SPAWN, // decides how many to spawn this step (writes emitter->spawn_request)
    PARTICLE_PHASE_INIT,  // stamps newly spawned particles [begin,end)
    PARTICLE_PHASE_UPDATE // advances all live particles [begin,end) = [0,count)
} ParticleModulePhase;

// Phase-appropriate kernel. SPAWN ignores [begin,end); INIT sees the new slice;
// UPDATE sees all live particles. dt is the fixed timestep, t the total time.
typedef void (*ParticleModuleFn)(struct ParticleModule* m, struct ParticleEmitter* e, size_t begin,
                                 size_t end, float dt, float t);

typedef struct ParticleModule {
    const char* name;
    ParticleModulePhase phase;
    ParticleModuleFn run;
    void (*free_fn)(struct ParticleModule* m);
    void* params; // concrete type private to the module's .c
} ParticleModule;

void free_particle_module(ParticleModule* m);

// --- Concrete v1 modules (spore demo needs these) ---

// SPAWN: emit `rate` particles per second (fractional remainder accumulated so
// rates below 1/dt still spawn).
ParticleModule* particle_module_spawn_rate(float rate);
// Retune an existing spawn-rate module. Setting 0 stops new spawns while the
// particles already alive finish their lives (a hard stop would pop them out).
void particle_module_spawn_rate_set(ParticleModule* m, float rate);

// INIT: uniformly random position inside the AABB [min,max].
ParticleModule* particle_module_init_box_location(vec3 min, vec3 max);
// INIT: lifetime uniform in [min_seconds, max_seconds].
ParticleModule* particle_module_init_lifetime(float min_seconds, float max_seconds);
// INIT: billboard half-size uniform in [min,max].
ParticleModule* particle_module_init_size(float min_size, float max_size);
// INIT: base HDR color, each rgb channel jittered by +/- rgb_jitter.
ParticleModule* particle_module_init_color(vec4 base_rgba, float rgb_jitter);

// UPDATE: divergence-free curl-noise turbulence advecting velocity. `scale` is
// the spatial frequency, `strength` the acceleration magnitude, `timescale` the
// rate the noise domain drifts over time (organic, non-repeating swirl).
ParticleModule* particle_module_update_curl_noise(float scale, float strength, float timescale);

// UPDATE: add a constant acceleration (m/s^2), e.g. faint upward buoyancy.
ParticleModule* particle_module_update_drift(vec3 accel);
// UPDATE: spin the billboard roll at a per-particle rate in [min,max] rad/s,
// direction alternating by seed (tumbling leaves, spinning embers). Only the
// CPU backend runs modules, so the transform-feedback backend ignores this.
ParticleModule* particle_module_update_rotation(float min_rate, float max_rate);

// UPDATE: integrate position by velocity and apply a per-step velocity damping
// factor `drag` in (0,1].
ParticleModule* particle_module_update_integrate(float drag);

// UPDATE: analytic collider -- ONE primitive; the shape and mode are data.
// Add AFTER update_integrate so it corrects the just-moved position.
//   shape: which analytic surface (extend with capsule/plane/... later -- a new
//          enum case + one kernel branch + one shader branch, no new module).
//   mode:  KEEP_OUT repels particles out of the shape (obstacles); KEEP_IN clamps
//          them inside it (containment). Orthogonal -- every shape supports both.
//   restitution: bounce of the into-surface velocity (0 = slide, 1 = elastic).
//   wake: KEEP_OUT only -- fraction of the shape's OWN velocity imparted to
//         touched particles, so a MOVING collider shoves dust along its travel.
typedef enum {
    COLLIDER_SPHERE,
    COLLIDER_BOX,
    COLLIDER_PLANE,   // half-space: particles stay on the +normal side
    COLLIDER_CAPSULE, // segment [p0,p1] swept by `radius` (rounded ends)
    COLLIDER_CYLINDER // finite cylinder: axis [p0,p1], `radius`, flat end caps
} ColliderShape;
typedef enum { COLLIDER_KEEP_OUT, COLLIDER_KEEP_IN } ColliderMode;

// Sphere collider at `center`, radius `radius`.
ParticleModule* particle_module_collider_sphere(vec3 center, float radius, ColliderMode mode,
                                                float restitution, float wake);
// Box (AABB) collider spanning [min,max].
ParticleModule* particle_module_collider_box(vec3 min, vec3 max, ColliderMode mode,
                                             float restitution);
// Infinite plane through `point` with unit `normal`: particles are kept on the
// +normal side (flip the normal for the other side). Ideal for a floor/wall.
ParticleModule* particle_module_collider_plane(vec3 point, vec3 normal, float restitution);
// Capsule: the segment [p0,p1] swept by `radius` (a cylinder with hemispherical
// ends). KEEP_OUT = an obstacle; KEEP_IN = a capsule-shaped bound.
ParticleModule* particle_module_collider_capsule(vec3 p0, vec3 p1, float radius, ColliderMode mode,
                                                 float restitution, float wake);
// Finite cylinder: axis [p0,p1], `radius`, with two flat end caps.
ParticleModule* particle_module_collider_cylinder(vec3 p0, vec3 p1, float radius, ColliderMode mode,
                                                  float restitution, float wake);
// Move a collider's shape for the next step (a moving obstacle). The module
// tracks the previous placement internally to derive the wake velocity. `a`/`b`
// are the shape's anchors: sphere `a`=center; box `a`/`b`=min/max; plane
// `a`=point,`b`=normal; capsule/cylinder `a`/`b`=endpoints (`radius` as given).
void particle_module_collider_set(ParticleModule* m, vec3 a, vec3 b, float radius);

// --- GPU-backend introspection ---
// The transform-feedback backend reuses the CPU spawn/init modules verbatim but
// runs the UPDATE phase on the GPU, so it needs the update modules' parameters as
// shader uniforms. These readers extract them without exposing the params'
// concrete (module-private) types. Each returns true and fills the out-params iff
// `m` is that update module (matched by name); otherwise false, out-params
// untouched.
bool particle_module_read_curl(const ParticleModule* m, float* scale, float* strength,
                               float* timescale);
bool particle_module_read_drift(const ParticleModule* m, vec3 accel_out);
bool particle_module_read_integrate(const ParticleModule* m, float* drag);

// A collider's full state, resolved for the GPU backend. `vel` is the shape's
// translation velocity this step ((placement - prev)/dt), used for the wake.
// For a sphere, `a` = center; for a box, `a`/`b` = min/max.
typedef struct {
    int shape; // ColliderShape
    int mode;  // ColliderMode
    vec3 a, b;
    vec3 vel;
    float radius;
    float restitution;
    float wake;
} ColliderState;
// Reads any collider module (matched on the run-fn pointer); `dt` derives `vel`.
bool particle_module_read_collider(const ParticleModule* m, ColliderState* out, float dt);

#endif // _PARTICLE_MODULE_H_
