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
 * ik_foot_set_ground (or ik_foot_set_target, for a point the ankle itself should reach)
 * with plain vectors in the skeleton's model space.
 */

typedef struct IkFootParams {
    // How far the pelvis may descend for a foot that cannot reach, as a fraction of that
    // foot's own leg length -- the same units plant_fraction uses, and for the same
    // reason: it carries across rigs and scales where a metre does not. 0 disables the
    // drop entirely.
    float max_pelvis_drop;
    float teleport_distance; // a target jumping farther than this snaps instead of blending
    // Seconds over which a transition's discontinuity is blended away (spec 12.9). The
    // offset at a lock or a release is captured in position AND velocity and decays
    // through cubic basis weights, so once it has decayed the applied target IS the
    // requested one. A first-order ease never reaches that: against a target moving at
    // the walk speed it sits a constant distance behind for as long as the motion lasts,
    // which measured 0.039 m and was the whole of the slide left after the lock landed.
    // 0 applies the requested target verbatim.
    float transition_time;
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

    // Contact labelling (spec 12.9). A foot is in contact when its ankle is within
    // contact_height of the ground it was handed AND rising or falling slower than
    // contact_speed -- both fractions of leg length, the second per second, so they
    // carry across rigs.
    //
    // Holden's method labels contacts OFFLINE from toe speed and height, on clips that
    // carry root motion. Ours do not: the character controller moves the entity, so a
    // stance foot's model-space speed is the walk speed rather than zero and a
    // horizontal threshold would label nothing. The vertical pair says the same thing
    // about a foot that has been set down and not yet picked up, needs no authored
    // data and no baking step, and is what the deviation costs.
    float contact_height;
    float contact_speed;

    // Locking (spec 12.9). A labelled contact locks when the solve is displacing the
    // foot by less than lock_distance, and releases when the label ends or the clip has
    // carried the foot more than unlock_distance from the point that was frozen. Both
    // fractions of leg length. 0 in lock_distance disables locking outright, which is
    // what leaves planting's behaviour reachable.
    //
    // unlock_distance is deliberately the LARGER, and that gap is the whole hysteresis:
    // with one threshold the decision is re-made from scratch every frame and can flip,
    // which is what "scissoring in and out very quickly" looks like.
    float lock_distance;
    float unlock_distance;
} IkFootParams;

typedef struct IkFoot {
    // ENGINE-OWNED: read freely, never write.
    int hip_index;
    int knee_index;
    int ankle_index;
    // The bone the contact is judged at and, from spec 12.9, held at. Defaults to the
    // ankle, which is what a rig with no toe bone has to use; ik_foot_set_toe moves it
    // to the real thing. The toe is the part in contact roughly nine times out of ten,
    // and leaving the heel free is what preserves the animation.
    int toe_index;
    vec3 pole_local;      // knee-forward, in the HIP's frame, so a turning hip carries it
    vec3 fallback_axis;   // bend axis from the bind pose, for a leg aimed along the pole
    vec3 applied_target;  // what the last solve actually used, after easing
    float applied_weight; // and the weight it applied, after the release fade
    // False until this foot's first solve, and PER FOOT rather than the system's
    // needs_reset because only a per-foot latch can express it: a foot registered after
    // a solve has applied_target still zeroed to the model origin, and easing onto its
    // real target from there walks the leg across the world. needs_reset is system-wide
    // and re-arms feet that are already settled.
    bool has_applied;

    // The contact label this solve settled, and the state it is derived from (spec
    // 12.9). Read it to ask whether the clip has this foot on the ground; it is a
    // statement about the ANIMATION, not about the solve, and it is answered here for
    // the same reason the release is: only at this point in the frame do the globals
    // still hold the clip's own pose.
    bool in_contact;
    vec3 clip_ankle_prev; // the ankle's model position at the previous solve
    bool has_clip_prev;   // false until there are two solves to difference

    // The held contact (spec 12.9): where the TOE was pinned, in WORLD space, and
    // whether it is pinned at all. World and not model, which is the entire point --
    // a model-space point travels with the character, which is what planting already
    // did and what a lock exists to stop. It costs the system a model-to-world matrix
    // the caller has to supply; see ik_set_world.
    bool locked;
    vec3 contact_world;
    // True for the one frame between deciding to lock and having a point to lock to. The
    // contact is captured AFTER the solve, from where the foot actually ended up, because
    // the decision is made from the clip's pose and the frame then moves the foot off it
    // -- captured before, the lock spends its first ticks dragging the foot onto a point
    // it was never at, which is travel across the ground and reads as exactly the slide
    // the feature exists to remove.
    bool contact_pending;
    // The inertialization (spec 12.9): the discontinuity captured at the last transition
    // and how long it has been decaying. offset_vel is the half a position-only blend
    // drops, and dropping it is why a foot used to arrive at a lock with the wrong speed
    // and be dragged straight afterwards.
    vec3 offset_pos;
    vec3 offset_vel;
    float since_transition;
    vec3 applied_prev; // the last solve's applied target, for the output's own velocity
    vec3 want_prev;    // and the last requested one, for the input's
    bool has_want_prev;
    // Everything hanging off the ankle -- toes, and whatever else a rig puts there --
    // resolved once at ik_add_foot. The solve rotates these rigidly with the ankle, or
    // they keep the pose the clip gave them while the ankle moves out from under them.
    // Nothing could see that before this spec: no rig in the corpus had a bone below an
    // ankle, so the subtree was always empty.
    uint8_t below_ankle[MAX_BONES];

    // SETTINGS: plain stores, written directly at any time.

    // How far this ankle rides above its sole. Unlike its neighbours this is a per-rig
    // CONSTANT, derived once at ik_add_foot from the bind pose -- overwrite it only for
    // a rig whose bind sole does not rest at model y = 0, which is the assumption that
    // makes it derivable at all. Applied by ik_foot_set_ground and never by
    // ik_foot_set_target, which takes a point for the ankle itself.
    //
    // It lives here rather than in the three callers that each derived it from the same
    // bind pose -- one of which carried a comment warning that it had to agree with
    // another.
    float sole_offset;
    // Model space, where the ANKLE should land. sole_offset is already folded in when it
    // was set through ik_foot_set_ground, so this is the ankle point either way.
    vec3 target;
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
    // Where this rig stands. Identity until ik_set_world, under which world space IS
    // model space -- correct for a rig that never moves and wrong for one that does, in
    // the specific way that makes a lock do nothing at all.
    mat4 model_to_world;
    mat4 world_to_model;
    IkFootParams params; // shared by every foot
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

// The toe of a registered foot: where contact is judged and, once locked, held. Must be
// a child of that foot's ankle. Refused when the bone is missing or is parented
// elsewhere, leaving the foot judged at its ankle, which is what a rig with no toe bone
// gets and is a real limitation rather than a simplification.
bool ik_foot_set_toe(IkSystem* system, int foot, const char* toe_bone);

// The bone a foot that cannot reach lowers. Refused when the bone is missing, or is
// not an ancestor of every registered foot.
bool ik_set_pelvis(IkSystem* system, const char* pelvis_bone);

// Where the ANKLE should land, verbatim. The raw form, for a caller that has a point
// it wants the ankle at.
void ik_foot_set_target(IkSystem* system, int foot, const vec3 target, const vec3 normal,
                        float weight);

// Where the GROUND is: the same thing with sole_offset added, so the foot stands ON the
// surface rather than sinking into it by its own thickness.
//
// Two entry points rather than one flag, because a target and a ground are different
// claims and only the caller knows which it holds -- often a line apart: a planting
// caller hands over the surface, and the same caller releasing a foot re-targets the
// ankle's own current position at weight 0 and must NOT have a clearance added to it.
// Folding the offset into ik_foot_set_target was tried and is wrong: most of its call
// sites pass arbitrary geometry, and a sole clearance displaced every one of them.
void ik_foot_set_ground(IkSystem* system, int foot, const vec3 ground, const vec3 normal,
                        float weight);

// Where the skeleton's model space sits in the world, for the frame about to be solved.
// A locked foot holds a WORLD point, so a caller that moves the rig must set this every
// frame or the held point travels with the character and the lock does nothing. Inverted
// once here rather than per foot.
void ik_set_world(IkSystem* system, mat4 model_to_world);

// Snap every foot to its target on the next solve, instead of easing to it. Also drops
// every held contact: after a teleport the world point a foot was pinned to is somewhere
// the character no longer is.
void ik_reset(IkSystem* system);

// Correct the accumulated pose. Globals only -- nothing re-accumulates after this
// point, so a local written here would have no reader. Deliberately takes no
// local_transforms: spring_bone_update needs them because it re-accumulates, and this
// does not.
void ik_solve(IkSystem* system, mat4* global_transforms, float delta_time);

#endif // _IK_H_
