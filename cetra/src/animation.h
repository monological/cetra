#ifndef _ANIMATION_H_
#define _ANIMATION_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ext/uthash.h"

#define MAX_BONES        128
#define BONES_PER_VERTEX 4

// Forward declarations
struct Mesh;
struct Scene;

// --- Bone ---

typedef struct Bone {
    char* name;
    int parent_index;       // -1 for root bones
    mat4 inverse_bind_pose; // Transforms from model space to bone space
    mat4 local_transform;   // Default local transform (bind pose)
} Bone;

// Hash entry for bone name -> index lookup
typedef struct BoneIndexEntry {
    char* name;
    int index;
    UT_hash_handle hh;
} BoneIndexEntry;

// --- Skeleton ---

typedef struct Skeleton {
    char* name;
    Bone* bones; // Flat array, ordered parent-first
    size_t bone_count;
    BoneIndexEntry* bone_map; // Name-to-index hash map
    UT_hash_handle hh;        // For skeleton caching by name
} Skeleton;

// Skeleton functions
Skeleton* create_skeleton(const char* name);
void free_skeleton(Skeleton* skeleton);
int add_bone_to_skeleton(Skeleton* skeleton, const char* name, int parent_index,
                         mat4 inverse_bind_pose, mat4 local_transform);
int get_bone_index_by_name(Skeleton* skeleton, const char* name);
Bone* get_bone_by_name(Skeleton* skeleton, const char* name);
Bone* get_bone_by_index(Skeleton* skeleton, int index);
void recalculate_inverse_bind_poses(Skeleton* skeleton);

// Accumulate bind-pose global transforms parent-first into globals
// (caller provides bone_count entries)
void skeleton_compute_bind_globals(Skeleton* skeleton, mat4* globals);

// Mark root and every bone descended from it, 1 in out[] and 0 elsewhere; returns how
// many were marked. out holds bone_count entries -- which add_bone_to_skeleton bounds at
// MAX_BONES, so a MAX_BONES array is always enough -- and is written in full, so a caller
// needs no clear of its own.
//
// One forward pass, because bones are ordered parent-first: a bone's parent is already
// decided when the loop reaches it. The parent < i test is what makes that safe to
// state rather than assume -- a skeleton whose order is violated gets a bone excluded
// rather than reading a slot the pass has not written yet.
//
// The root is an INDEX and not a name: the two callers resolve it differently, one
// from a name with its own log line and one from a bone it already holds, and folding
// the lookup in here would force a name on the caller that does not have one.
size_t skeleton_mark_subtree(const Skeleton* skeleton, int root, uint8_t* out);

// Rotate a bone's global by q about `head`, which the caller states rather than the
// function reading it back: for a mid-chain joint the head is where the bone WILL be,
// not where it currently is. dest and src may be the same matrix.
//
// static inline, deliberately. The build pins no -ffp-contract, so whether the compiler
// contracts glm_mat4_mul's multiply-add chains into FMAs can follow the inlining context
// -- and both call sites currently have this inlined into them. An extern in animation.c
// would change that context for a function whose output feeds skinning matrices, to buy
// nothing.
static inline void skeleton_rotate_global(mat4 dest, mat4 src, versor q, const vec3 head) {
    mat4 rot;
    glm_quat_mat4(q, rot);
    glm_mat4_mul(rot, src, dest);
    dest[3][0] = head[0];
    dest[3][1] = head[1];
    dest[3][2] = head[2];
    dest[3][3] = 1.0f;
}

// --- Keyframes ---

typedef struct PositionKey {
    float time;
    vec3 position;
} PositionKey;

typedef struct RotationKey {
    float time;
    versor rotation; // Quaternion (cGLM's versor type)
} RotationKey;

typedef struct ScaleKey {
    float time;
    vec3 scale;
} ScaleKey;

// --- Animation Channel ---

typedef struct AnimationChannel {
    int bone_index;  // Index into skeleton->bones[]
    char* bone_name; // For debugging/lookup

    PositionKey* position_keys;
    size_t position_key_count;

    RotationKey* rotation_keys;
    size_t rotation_key_count;

    ScaleKey* scale_keys;
    size_t scale_key_count;

    // Retargeting support
    // Set for every matched bone, not only a mismatched one: it is also what makes
    // the sample take its POSITION from the bind pose rather than from the clip.
    bool needs_retargeting;
    versor rotation_delta; // Correction quaternion: inv(source_rest) * target_rest,
                           // applied on the right, so keyframe * delta reads as
                           // "pose, undo the source rest, apply the target's"

    // Global-space retargeting (when source skeleton is provided)
    bool use_global_retarget;     // True when source skeleton hierarchy is available
    int source_bone_index;        // Index in source skeleton (-1 if unmatched)
    int source_parent_bone_index; // Source parent's index in source skeleton
    versor source_local_rest;     // Source bone's local rest rotation
} AnimationChannel;

// Channel functions
AnimationChannel* create_animation_channel(int bone_index, const char* bone_name);
void free_animation_channel(AnimationChannel* channel);
int add_position_key(AnimationChannel* channel, float time, vec3 position);
int add_rotation_key(AnimationChannel* channel, float time, versor rotation);
int add_scale_key(AnimationChannel* channel, float time, vec3 scale);

// --- Animation ---

// A marker on a clip's timeline (spec 12.1): a footstep, a hit window opening,
// a point a game wants to hear about. What fires it is the Animator; a clip
// only carries it.
typedef struct AnimationEvent {
    float time_ticks;
    char* name; // owned
} AnimationEvent;

typedef struct Animation {
    char* name;
    float duration;         // In ticks
    float ticks_per_second; // Typically 24, 30, or 60

    AnimationChannel* channels;
    size_t channel_count;

    AnimationEvent* events; // animation_add_event; sorted by time
    size_t event_count;

    Skeleton* skeleton; // Associated skeleton (pointer, not owned)

    UT_hash_handle hh; // For animation caching by name
} Animation;

// Animation functions
Animation* create_animation(const char* name, float duration, float ticks_per_second);
void free_animation(Animation* animation);
int add_channel_to_animation(Animation* animation, AnimationChannel* channel);
// Insert a timeline event, kept sorted by time so a frame's crossings dispatch
// in clip order. A time outside [0, duration] is refused by name. Returns 0, or
// -1 on refusal or OOM.
int animation_add_event(Animation* animation, float time_ticks, const char* name);
AnimationChannel* get_channel_for_bone(const Animation* animation, int bone_index);
AnimationChannel* get_channel_for_bone_name(Animation* animation, const char* bone_name);

// --- Animation State ---

struct SpringBoneSystem;
struct IkSystem;

// A skeleton's live pose and the buffers a frame skins from. WHAT PLAYS is
// not here -- an Animator owns the clock, the blend and the fades (animator.h),
// and hands this a finished Pose. A node points at one of these (node_set_pose)
// and every skinned mesh under it draws from these matrices.
typedef struct AnimationState {
    Skeleton* skeleton;

    // Computed bone matrices (global transform * inverse bind pose)
    mat4 bone_matrices[MAX_BONES];
    // Previous frame's bones for skinned motion vectors (TAA), packed as 3 affine
    // rows per bone (upload-ready — the implicit 4th row is 0,0,0,1). Filled by
    // animation_snapshot_prev_pose once per frame, before the pose is rebuilt.
    float prev_bone_rows[3 * MAX_BONES * 4];
    size_t active_bone_count;

    // Scratch space for transform computation
    mat4* local_transforms;  // Per-bone local transforms (interpolated)
    mat4* global_transforms; // Per-bone global transforms (accumulated)

    // Optional spring-bone secondary motion (see springbone.h)
    struct SpringBoneSystem* springs;

    // Optional two-bone IK, applied after the springs (see ik.h)
    struct IkSystem* ik;

    // Optional ragdoll, applied after the IK and REPLACING the pose rather than
    // correcting it (see ragdoll.h). Owned from assignment, like the two above.
    struct RagdollSystem* ragdoll;

    // Dump the first animated pose to stdout: per bone, whether the clip drives
    // it at all, its bind and animated global positions, the distance between
    // them, and the skinning matrix's column scales. Off by default.
    //
    // The two it exists to catch look nothing alike. A bone the retargeter
    // failed to map has ch=0 and a large drift -- it is being carried by its
    // parent instead of animated. A scale that is not 1 is a corrupt matrix,
    // which reads downstream as geometry that stretches to infinity rather than
    // as a pose that is merely wrong.
    //
    // PER STATE, not per process. It was a file-static counter, so a scene with
    // two rigs dumped the first one and silently skipped the second -- and the
    // second is the one you are usually asking about, since the first is the
    // model that already worked.
    bool debug_pose_dump;
    bool debug_pose_dumped; // one-shot latch for the above
} AnimationState;

// Animation state functions
AnimationState* create_animation_state(Skeleton* skeleton);
void free_animation_state(AnimationState* state);
// Snapshot the current bone matrices into prev_bone_rows (packed affine rows)
// for next frame's skinned motion vectors. Once per frame, BEFORE the pose is
// rebuilt, and unconditionally -- so a paused pose reads zero deformation
// velocity rather than a frozen nonzero one. animator_update begins with it.
void animation_snapshot_prev_pose(AnimationState* state);

// --- Keyframe Interpolation ---

void interpolate_position(PositionKey* keys, size_t count, float time, vec3 out);
void interpolate_rotation(RotationKey* keys, size_t count, float time, versor out);
void interpolate_scale(ScaleKey* keys, size_t count, float time, vec3 out);

// --- Pose (spec 12.1) ---
//
// A skeleton's local pose as VALUES: per bone a position, a rotation and a
// scale. Matrices are not blendable and this is, which is the whole reason it
// exists -- a clip is sampled into one, two of them are blended, and the result
// is applied to an AnimationState, where it becomes the skinning matrices.

typedef struct BoneTransform {
    vec3 position;
    versor rotation; // what the clip's interpolation produced, not renormalised
    vec3 scale;
} BoneTransform;

typedef struct Pose {
    const Skeleton* skeleton; // what bones[] indexes; set by every producer
    size_t bone_count;
    BoneTransform bones[MAX_BONES];
    // 0 = no clip drove this bone. Its TRS holds the bind local DECOMPOSED so a
    // blend against it has numbers, but applying it copies the bind MATRIX --
    // an affine decomposed and rebuilt is not the same bits, and a bone no clip
    // touches has to skin exactly as it did before there was a Pose.
    //
    // That exactness is the SINGLE-SOURCE path's. pose_blend marks a bone
    // driven if EITHER side drove it, so the moment a blend touches a bone the
    // undriven side contributes its bind local decomposed and recomposed. That
    // is unavoidable -- a matrix cannot be blended -- and it is why the identity
    // the migration rests on is claimed for one clip and not for a fade.
    uint8_t driven[MAX_BONES];
} Pose;

// Every bone at its bind local, driven = 0 throughout.
void pose_bind(const Skeleton* skeleton, Pose* out);

// ONE clip at one time, self-contained: the retargeting and the hierarchy it
// needs are evaluated in scratch inside the call, so two clips sampled in turn
// cannot see each other. A NULL clip is the bind pose; a clip bound to another
// skeleton is refused by name and reads as bind.
void animation_sample_pose(const Animation* anim, const Skeleton* skeleton, float time_ticks,
                           Pose* out);

// The ground speed this clip's own feet imply at playback rate 1, in MODEL units per
// second, and the direction the body must travel for them to be stationary (may be NULL).
// `ankle` and `toe` are the two feet's bone indices, left and right in either order.
//
// MODEL units, so a caller whose rig sits on a scaled node owes the scale -- the same
// division of labour `ik_set_world` states, and the same one that silently halved a
// threshold in spec 12.9.
//
// False when the clip does not walk, which is a real answer about the clip and not a
// failure to measure: a stance has to exist (each foot dwelling once per loop, the two
// alternating) before "how fast does the ground go past it" means anything. A pendulum
// swing where the foot is lowest exactly where it is fastest has no such window, and
// fitting one anyway yields a plausible number pointing the wrong way.
bool animation_stride_speed(const Animation* clip, const Skeleton* skeleton, const int ankle[2],
                            const int toe[2], float* out_speed, vec3 out_dir);

// out = a at t = 0, b at t = 1: positions and scales lerped, rotations nlerped
// along the shorter arc. Outside (0, 1) the nearer pose is copied, flags
// included. A bone undriven in both stays undriven. out may alias a or b.
void pose_blend(const Pose* a, const Pose* b, float t, Pose* out);

// The same per bone with its own weight, bone_weights[bone_count]: <= 0 keeps
// base, >= 1 takes over, between blends. out may alias either.
void pose_blend_masked(const Pose* base, const Pose* over, const float* bone_weights, Pose* out);

// The pose becomes the state's locals, globals, spring-bone result and skinning
// matrices. delta_time drives the springs only (0 poses without advancing them).
// A pose for another skeleton is refused by name and leaves the state alone.
void animation_state_apply_pose(AnimationState* state, const Pose* pose, float delta_time);

// --- Bone Matrix Computation ---

// The bind pose into the state's matrices, springs untouched. What a state
// holds before anything has played through it.
void compute_bind_pose_matrices(AnimationState* state);

// --- Debug ---

void print_skeleton(const Skeleton* skeleton);
void print_animation(const Animation* animation);
void print_animation_state(const AnimationState* state);

#endif // _ANIMATION_H_
