#ifndef _RAGDOLL_H_
#define _RAGDOLL_H_

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "animation.h"
#include "ragdoll_jolt.h"

struct Mesh;

/*
 * A rig falling over (spec 12.16): a dozen physics bodies built from a
 * skeleton's BIND pose, started from its LIVE pose, and written back over the
 * accumulated globals.
 *
 * The seam is ik.c's, and for ik.c's reason. ragdoll_apply takes
 * global_transforms and nothing else, because nothing re-accumulates after the
 * point it is spliced in -- so a local written here would have no reader, and
 * writing the Pose instead would be discarded outright for every bone no clip
 * drives, which ik.h states at length.
 *
 * Everything measured here is MODEL space, in the units the skeleton's bind
 * pose is in, and the owning node's scale is applied exactly once at build.
 *
 * LIFETIME. A live ragdoll holds bodies inside a JPH::PhysicsSystem and cannot
 * be destroyed without one, so whatever owns a ragdoll must free it before the
 * physics world. An AnimationState owning one is safe under the game framework,
 * where free_game tears the entity manager down before the world -- it is not
 * safe for a caller that frees a physics world first, and nothing here can
 * check that.
 */

// The bones a humanoid ragdoll simulates. Order is the BUILD order and is
// parent-first, which the Jolt layer requires and checks.
typedef enum RagdollBone {
    RAGDOLL_HIPS = 0,
    RAGDOLL_SPINE,
    RAGDOLL_CHEST,
    RAGDOLL_HEAD,
    RAGDOLL_UPPER_ARM_L,
    RAGDOLL_LOWER_ARM_L,
    RAGDOLL_UPPER_ARM_R,
    RAGDOLL_LOWER_ARM_R,
    RAGDOLL_THIGH_L,
    RAGDOLL_SHIN_L,
    RAGDOLL_THIGH_R,
    RAGDOLL_SHIN_R,
    RAGDOLL_BONE_COUNT,
} RagdollBone;

typedef struct RagdollSystem RagdollSystem;

/*
 * Resolve the humanoid bones on `skeleton`, measure a capsule for each from the
 * bind pose and the mesh's per-bone bounds, and report what was found. No
 * physics and no allocation beyond the system itself: this is the half that can
 * be asked what it would build.
 *
 * `mesh` may be NULL, in which case every radius falls back to a fraction of
 * its own bone's length -- thinner than measured limbs and never zero, so a
 * skeleton with no skin still produces a legal ragdoll.
 *
 * A row this rig does not answer COLLAPSES rather than leaving a hole -- a
 * missing chest re-points the arms and the head at the spine -- which is what
 * makes the body count a property of the rig instead of a constant. What
 * cannot collapse is REFUSED by name: no hips, or no thigh, is not a humanoid
 * this can build, and a ragdoll assembled around that gap simulates perfectly
 * and looks like a bug in the solver.
 */
RagdollSystem* create_ragdoll(Skeleton* skeleton, const struct Mesh* mesh, float node_scale);
void free_ragdoll(RagdollSystem* ragdoll);

// The slot's own name -- a property of the humanoid this describes rather than
// of any rig, so it answers without one.
const char* ragdoll_bone_name(RagdollBone which);

// Which skeleton bone a ragdoll body came from, or -1. Diagnostics and arms.
int ragdoll_bone_index(const RagdollSystem* ragdoll, RagdollBone which);
// Which SLOT this one's body hangs from, after the rows this rig did not answer
// have collapsed out -- a missing chest re-points the arms and the head at the
// spine. -1 for the root and for a slot with no body.
int ragdoll_bone_parent(const RagdollSystem* ragdoll, RagdollBone which);
// The capsule measured for one body, in MODEL units times the node scale.
bool ragdoll_capsule(const RagdollSystem* ragdoll, RagdollBone which, float* out_radius,
                     float* out_half_height);

/*
 * Start simulating, from the pose `globals` currently holds. `to_world` is the
 * owning node's world matrix -- the same matrix ik_set_world takes, and for the
 * same reason: bone globals are model space and bodies are world space.
 *
 * Takes the opaque physics handle and the object layer rather than a game-layer
 * world, so the engine half names nothing from the layer above it. The caller
 * already holds both.
 *
 * False, logged, if the bodies could not be built; the caller stays animated.
 */
bool ragdoll_start(RagdollSystem* ragdoll, JPC_PhysicsSystem* system, uint32_t object_layer,
                   const mat4* globals, const mat4 to_world);
bool ragdoll_active(const RagdollSystem* ragdoll);

// The owning node's world matrix, which moves while the character does. Read at
// apply time, so a caller sets it whenever it changes.
void ragdoll_set_world(RagdollSystem* ragdoll, const mat4 to_world);

/*
 * Overwrite the accumulated globals with the simulated pose. A simulated bone
 * takes its body's transform; every other bone re-accumulates from its parent
 * using its BIND local, so a hand keeps its shape on the end of a forearm.
 *
 * A no-op while inactive, which is what lets the caller splice it in
 * unconditionally.
 */
void ragdoll_apply(RagdollSystem* ragdoll, mat4* global_transforms);

// Where the body is, for a camera or a controller to follow. False while
// inactive.
bool ragdoll_hips_world(const RagdollSystem* ragdoll, vec3 out);

#endif // _RAGDOLL_H_
