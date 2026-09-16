#ifndef _RAGDOLL_JOLT_H_
#define _RAGDOLL_JOLT_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Jolt's ragdoll behind a C header (spec 12.16). The third one-TU split, after
 * cluster_build.cpp and physics_cook.cpp, and for the same reason: JPH::Ragdoll
 * and the whole JPH::Skeleton tree are compiled into this binary and JoltC binds
 * none of them -- a grep for Ragdoll or Skeleton across JoltC returns nothing.
 * Lives in cetra/src/ root because the build globs C++ sources at the src root
 * and only C sources under game/, so a .cpp under game/ would silently not
 * compile.
 *
 * What this does NOT wrap is as deliberate as what it does. There is no pose
 * driving here -- no SetPose, no DriveToPoseUsingMotors -- because a ragdoll
 * that can be driven back toward a clip is the getting-up problem, which 12.16
 * declined. The constraint type below is chosen so that stays possible.
 */

typedef struct JPC_PhysicsSystem JPC_PhysicsSystem; // matches JoltC/Functions.h's opaque type
typedef struct JoltRagdoll JoltRagdoll;

/*
 * One simulated body. The caller fills an array of these, PARENT-FIRST.
 *
 * That ordering is not a convenience: JPH::Skeleton documents it
 * (AreJointsCorrectlyOrdered) because CreateRagdoll indexes the parent's body
 * while building the child, so a child listed first reads a body that does not
 * exist yet. jolt_ragdoll_create refuses an array that violates it rather than
 * letting Jolt walk off the end.
 *
 * The capsule is along the body's own local Y and centred on it, so `world`
 * places the MIDDLE of the limb, not its joint. The caller does that conversion
 * because only the caller knows where the bone's child sits.
 */
typedef struct RagdollBuild {
    const char* name; // borrowed, diagnostics only
    int parent;       // index into this same array; -1 for the root
    float capsule_radius;
    float capsule_half_height; // half the CYLINDER, excluding the two caps
    mat4 world;                // where this body starts

    // The swing-twist limits, in degrees, applied to the constraint joining this
    // body to its parent. Ignored for the root, which has no constraint.
    float cone_deg;  // half-angle about the twist axis, normal direction
    float plane_deg; // half-angle about the twist axis, plane direction
    float twist_min_deg;
    float twist_max_deg;
} RagdollBuild;

/*
 * Build the bodies and constraints and add them to the world. NULL on any
 * refusal, logged by name: no bodies, a bad parent index, a child before its
 * parent, or the body pool exhausted.
 *
 * `group_id` must be unique per ragdoll in one PhysicsSystem -- Jolt uses it as
 * the collision group that stops a limb colliding with its own parent, so two
 * ragdolls sharing an id stop colliding with each other's limbs.
 */
JoltRagdoll* jolt_ragdoll_create(JPC_PhysicsSystem* system, const RagdollBuild* parts, int count,
                                 uint32_t group_id);

// Removes from the world and destroys the bodies. Both halves matter and in
// that order -- ~Ragdoll destroys bodies without removing them, so releasing a
// ragdoll still in the broadphase destroys bodies the world is still indexing.
void jolt_ragdoll_destroy(JoltRagdoll* ragdoll);

int jolt_ragdoll_body_count(const JoltRagdoll* ragdoll);

// The body's current world transform, rotation and translation only. False and
// unwritten for an out-of-range index.
bool jolt_ragdoll_get_world(const JoltRagdoll* ragdoll, int index, mat4 out);

// An impulse on every body, which is what a death blow looks like from outside.
void jolt_ragdoll_add_impulse(JoltRagdoll* ragdoll, const vec3 impulse);

// JPH_VERSION_ID, evaluated where the C++ headers are visible -- physics_cook.h
// exports the same value for the cook key, and this exists so a probe can
// assert the two TUs were built against one Jolt.
uint64_t jolt_ragdoll_jolt_version(void);

#ifdef __cplusplus
}
#endif

#endif // _RAGDOLL_JOLT_H_
