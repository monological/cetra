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
    float max_pelvis_drop;   // how far the pelvis may descend for an out-of-reach foot, metres
    float teleport_distance; // a target jumping farther than this snaps instead of easing
    float blend_rate;        // 1/seconds the applied target eases toward the requested one
    // How far above its target an animated foot may be and still be planted, as a
    // fraction of the leg's own length, so it carries across rigs and scales. Past it
    // the clip has lifted the foot deliberately and the solve lets go.
    //
    // This decision lives in the SOLVER and not in the caller, and that is not taste. A
    // caller can only read the globals from before its own frame, which by then hold
    // the LAST solve -- so a planted foot reads as already planted, holds, and the
    // stride dies with the feet welded to the ground. Here the globals are still the
    // clip's own pose for this frame, which is the only moment the question has a true
    // answer. Zero disables the release entirely and plants at full weight.
    float plant_fraction;
} IkFootParams;

typedef struct IkFoot {
    // ENGINE-OWNED: read freely, never write.
    int hip_index;
    int knee_index;
    int ankle_index;
    vec3 pole_local;      // knee-forward, in the HIP's frame, so a turning hip carries it
    vec3 fallback_axis;   // bend axis from the bind pose, for a leg aimed along the pole
    vec3 applied_target;  // what the last solve actually used, after easing
    float applied_weight; // and the weight it applied, after the release fade
    // False until this foot's first solve, and the reason it is PER FOOT rather than
    // the system's needs_reset: a foot registered after a solve has applied_target
    // zeroed to the model origin, and easing onto its real target from there walks the
    // leg across the world. Arming system->needs_reset instead was measured moving the
    // menu golden 672 px, so the distinction is load-bearing and not bookkeeping.
    bool has_applied;

    // SETTINGS: plain stores, written every frame by whoever knows the ground.
    vec3 target; // model space, where the ankle should land
    // The surface the foot stands on, model space. Stated by the caller because it is
    // part of the contract, and currently READ BY NOTHING: the solve does not yet pitch
    // the sole onto it, which wants a sole axis this rig does not state, and guessing
    // one is worse than leaving the foot at the orientation its clip gave it.
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
