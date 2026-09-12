#ifndef _IK_H_
#define _IK_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "animation.h"

/*
 * Two-bone inverse kinematics, for planting a foot on ground a clip knew nothing
 * about (spec 12.4). A chain of exactly two bones -- hip, knee, ankle -- is a
 * triangle with three known sides, so the solve is the law of cosines: exact,
 * non-iterative, and with no convergence knob. Longer chains want CCD or FABRIK
 * and are deliberately not here.
 *
 * This runs where spring bones run, AFTER them, between the pose's globals being
 * accumulated and the skinning matrices being built -- the one place per-bone
 * MODEL-space positions exist, which is what a law-of-cosines solve needs. It
 * writes global_transforms and NOTHING else: not the Pose, not the locals.
 *
 * Writing the Pose looks like the tidier design and does not work. A bone no clip
 * drives has driven[] clear, and pose_local then discards its TRS and copies the
 * bind matrix verbatim -- so on a rig whose locomotion never bends a knee, every
 * correction would be thrown away on every frame. Setting the bit instead forfeits
 * the decompose/recompose bit-identity the single-clip path rests on.
 *
 * Order against the springs is the other thing not to change casually.
 * spring_bone_update re-accumulates global = parent_global * local for every
 * UNSIMULATED bone, and leg joints are unsimulated, so a solve placed before it is
 * erased. Running after costs one frame of lag in a spring's swing target and
 * nothing in its position, because a pelvis drop translates simulated bones too.
 *
 * The engine half knows nothing about physics. Where the ground is, and whether a
 * foot is on it at all, are the app's to answer -- it raycasts and calls
 * ik_foot_set_target with plain vectors in the skeleton's model space.
 */

typedef struct IkFootParams {
    float reach_limit;       // fraction of (thigh + shin) a chain may extend to; < 1 keeps a bend
    float max_pelvis_drop;   // how far the pelvis may descend for an out-of-reach foot, metres
    float teleport_distance; // a target jumping farther than this snaps instead of easing
    float blend_rate;        // 1/seconds the applied target eases toward the requested one
} IkFootParams;

typedef struct IkFoot {
    // ENGINE-OWNED: read freely, never write.
    int hip_index;
    int knee_index;
    int ankle_index;
    vec3 pole_local;     // knee-forward, in the HIP's frame, so a turning hip carries it
    vec3 fallback_axis;  // bend axis from the bind pose, for a leg aimed along the pole
    vec3 applied_target; // what the last solve actually used, after easing
    vec3 applied_normal;
    bool has_applied; // false until the first solve; the snap latch

    // SETTINGS: plain stores, written every frame by whoever knows the ground.
    vec3 target; // model space, where the ankle should land
    // The surface the foot stands on, model space. Carried and eased alongside the
    // target so a caller and a probe can read it, but the solve does NOT yet pitch the
    // sole onto it -- that wants a sole axis this rig does not state, and guessing one
    // is worse than leaving the foot at the orientation its clip gave it.
    vec3 normal;
    float weight; // 0 = the animated pose exactly (and bit-identical), 1 = full IK
} IkFoot;

typedef struct IkSystem {
    Skeleton* skeleton; // not owned
    IkFoot* feet;
    size_t foot_count;
    int pelvis_index;                     // -1 = no pelvis drop
    uint8_t in_pelvis_subtree[MAX_BONES]; // resolved once, at ik_set_pelvis
    IkFootParams params;                  // shared by every foot
    bool enabled;
    bool needs_reset; // snap every foot on the next solve
} IkSystem;

// Created with default params; adjust system->params directly to tune.
IkSystem* create_ik_system(Skeleton* skeleton);
void free_ik_system(IkSystem* system);

IkFootParams ik_default_params(void);

// Register one leg. The three bones must form a parent chain (hip -> knee -> ankle);
// knee_forward is the direction the knee should bend, in the HIP's frame. Returns the
// foot's index, or -1 if a name is missing or the bones are not a chain.
int ik_add_foot(IkSystem* system, const char* hip_bone, const char* knee_bone,
                const char* ankle_bone, const vec3 knee_forward);

// The bone a foot that cannot reach lowers. Refused when the bone is missing, or is
// not an ancestor of every registered foot.
bool ik_set_pelvis(IkSystem* system, const char* pelvis_bone);

void ik_foot_set_target(IkSystem* system, int foot, const vec3 target, const vec3 normal,
                        float weight);

// Snap every foot to its target on the next solve, instead of easing to it.
void ik_reset(IkSystem* system);

// Correct the accumulated pose. Globals only -- nothing re-accumulates after this
// point, so a local written here would have no reader. Deliberately takes no
// local_transforms: spring_bone_update needs them because it re-accumulates, and this
// does not.
void ik_solve(IkSystem* system, mat4* global_transforms, float delta_time);

#endif // _IK_H_
