#ifndef _ANIMATION_H_
#define _ANIMATION_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

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
} AnimationChannel;

// Channel functions
AnimationChannel* create_animation_channel(int bone_index, const char* bone_name);
void free_animation_channel(AnimationChannel* channel);
int add_position_key(AnimationChannel* channel, float time, vec3 position);
int add_rotation_key(AnimationChannel* channel, float time, versor rotation);
int add_scale_key(AnimationChannel* channel, float time, vec3 scale);

// --- Animation ---

typedef struct Animation {
    char* name;
    float duration;         // In ticks
    float ticks_per_second; // Typically 24, 30, or 60

    AnimationChannel* channels;
    size_t channel_count;

    Skeleton* skeleton; // Associated skeleton (pointer, not owned)

    UT_hash_handle hh; // For animation caching by name
} Animation;

// Animation functions
Animation* create_animation(const char* name, float duration, float ticks_per_second);
void free_animation(Animation* animation);
int add_channel_to_animation(Animation* animation, AnimationChannel* channel);
AnimationChannel* get_channel_for_bone(const Animation* animation, int bone_index);
AnimationChannel* get_channel_for_bone_name(Animation* animation, const char* bone_name);

// --- Animation State ---

typedef struct AnimationState {
    Animation* current_animation;
    Skeleton* skeleton;

    float current_time; // In ticks
    float speed;        // Playback speed multiplier (1.0 = normal)
    bool looping;
    bool playing;

    // Computed bone matrices (global transform * inverse bind pose)
    mat4 bone_matrices[MAX_BONES];
    size_t active_bone_count;

    // Scratch space for transform computation
    mat4* local_transforms;  // Per-bone local transforms (interpolated)
    mat4* global_transforms; // Per-bone global transforms (accumulated)
} AnimationState;

// Animation state functions
AnimationState* create_animation_state(Skeleton* skeleton);
void free_animation_state(AnimationState* state);
void set_animation(AnimationState* state, Animation* animation);
void play_animation(AnimationState* state);
void pause_animation(AnimationState* state);
void stop_animation(AnimationState* state);
void reset_animation(AnimationState* state);
void update_animation(AnimationState* state, float delta_time);

// --- Keyframe Interpolation ---

void interpolate_position(PositionKey* keys, size_t count, float time, vec3 out);
void interpolate_rotation(RotationKey* keys, size_t count, float time, versor out);
void interpolate_scale(ScaleKey* keys, size_t count, float time, vec3 out);

// --- Bone Matrix Computation ---

void compute_bone_matrices(AnimationState* state);
void compute_bind_pose_matrices(AnimationState* state);

// --- Debug ---

void print_skeleton(const Skeleton* skeleton);
void print_animation(const Animation* animation);
void print_animation_state(const AnimationState* state);

#endif // _ANIMATION_H_
