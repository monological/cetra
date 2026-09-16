#include "animation.h"
#include "ik.h"
#include "ragdoll.h"
#include "springbone.h"
#include "util.h"
#include "ext/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ============================================================================
// Skeleton
// ============================================================================

Skeleton* create_skeleton(const char* name) {
    Skeleton* skeleton = malloc(sizeof(Skeleton));
    if (!skeleton) {
        log_error("Failed to allocate memory for Skeleton");
        return NULL;
    }

    skeleton->name = safe_strdup(name);
    skeleton->bones = NULL;
    skeleton->bone_count = 0;
    skeleton->bone_map = NULL;

    return skeleton;
}

void free_skeleton(Skeleton* skeleton) {
    if (!skeleton)
        return;

    // Free bone names and bone map entries
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        if (skeleton->bones[i].name)
            free(skeleton->bones[i].name);
    }

    // Free bone map hash table
    BoneIndexEntry* entry;
    BoneIndexEntry* tmp;
    HASH_ITER(hh, skeleton->bone_map, entry, tmp) {
        HASH_DEL(skeleton->bone_map, entry);
        if (entry->name)
            free(entry->name);
        free(entry);
    }

    if (skeleton->bones)
        free(skeleton->bones);

    if (skeleton->name)
        free(skeleton->name);

    free(skeleton);
}

int add_bone_to_skeleton(Skeleton* skeleton, const char* name, int parent_index,
                         mat4 inverse_bind_pose, mat4 local_transform) {
    if (!skeleton || !name)
        return -1;

    if (skeleton->bone_count >= MAX_BONES) {
        log_error("Skeleton '%s' exceeded max bone limit (%d)", skeleton->name, MAX_BONES);
        return -1;
    }

    // Resize bones array
    size_t new_count = skeleton->bone_count + 1;
    Bone* new_bones = realloc(skeleton->bones, new_count * sizeof(Bone));
    if (!new_bones) {
        log_error("Failed to allocate memory for new bone");
        return -1;
    }
    skeleton->bones = new_bones;

    // Initialize new bone
    int bone_index = (int)skeleton->bone_count;
    Bone* bone = &skeleton->bones[bone_index];

    bone->name = safe_strdup(name);
    if (!bone->name) {
        log_error("Failed to allocate bone name");
        return -1;
    }
    bone->parent_index = parent_index;
    glm_mat4_copy(inverse_bind_pose, bone->inverse_bind_pose);
    glm_mat4_copy(local_transform, bone->local_transform);

    skeleton->bone_count = new_count;

    // Add to hash map for name lookup
    BoneIndexEntry* entry = malloc(sizeof(BoneIndexEntry));
    if (entry) {
        entry->name = safe_strdup(name);
        if (!entry->name) {
            free(entry);
        } else {
            entry->index = bone_index;
            HASH_ADD_KEYPTR(hh, skeleton->bone_map, entry->name, strlen(entry->name), entry);
        }
    }

    return bone_index;
}

int get_bone_index_by_name(Skeleton* skeleton, const char* name) {
    if (!skeleton || !name)
        return -1;

    BoneIndexEntry* entry = NULL;
    HASH_FIND_STR(skeleton->bone_map, name, entry);

    return entry ? entry->index : -1;
}

Bone* get_bone_by_name(Skeleton* skeleton, const char* name) {
    int index = get_bone_index_by_name(skeleton, name);
    if (index < 0)
        return NULL;
    return &skeleton->bones[index];
}

Bone* get_bone_by_index(Skeleton* skeleton, int index) {
    if (!skeleton || index < 0 || (size_t)index >= skeleton->bone_count)
        return NULL;
    return &skeleton->bones[index];
}

void skeleton_compute_bind_globals(Skeleton* skeleton, mat4* globals) {
    if (!skeleton || !globals)
        return;

    // Bones are ordered parent-first, so parent is always processed before child
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        if (bone->parent_index < 0 || (size_t)bone->parent_index >= skeleton->bone_count) {
            glm_mat4_copy(bone->local_transform, globals[i]);
        } else {
            glm_mat4_mul(globals[bone->parent_index], bone->local_transform, globals[i]);
        }
    }
}

size_t skeleton_mark_subtree(const Skeleton* skeleton, int root, uint8_t* out) {
    if (!skeleton || !out)
        return 0;

    size_t marked = 0;
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        const int parent = skeleton->bones[i].parent_index;
        const bool in = (int)i == root || (parent >= 0 && (size_t)parent < i && out[parent] != 0);
        out[i] = in ? 1 : 0;
        marked += in ? 1 : 0;
    }
    return marked;
}

void recalculate_inverse_bind_poses(Skeleton* skeleton) {
    if (!skeleton || skeleton->bone_count == 0)
        return;

    log_info("Recalculating inverse_bind_poses for skeleton '%s' (%zu bones)",
             skeleton->name ? skeleton->name : "unnamed", skeleton->bone_count);

    mat4* globals = malloc(skeleton->bone_count * sizeof(mat4));
    if (!globals)
        return;

    skeleton_compute_bind_globals(skeleton, globals);
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_inv(globals[i], skeleton->bones[i].inverse_bind_pose);
    }

    free(globals);
    log_info("Recalculation complete");
}

// ============================================================================
// Animation Channel
// ============================================================================

AnimationChannel* create_animation_channel(int bone_index, const char* bone_name) {
    AnimationChannel* channel = malloc(sizeof(AnimationChannel));
    if (!channel) {
        log_error("Failed to allocate memory for AnimationChannel");
        return NULL;
    }

    channel->bone_index = bone_index;
    channel->bone_name = safe_strdup(bone_name);

    channel->position_keys = NULL;
    channel->position_key_count = 0;

    channel->rotation_keys = NULL;
    channel->rotation_key_count = 0;

    channel->scale_keys = NULL;
    channel->scale_key_count = 0;

    // Initialize retargeting fields
    channel->needs_retargeting = false;
    glm_quat_identity(channel->rotation_delta);

    // Initialize global-space retargeting fields
    channel->use_global_retarget = false;
    channel->source_bone_index = -1;
    channel->source_parent_bone_index = -1;
    glm_quat_identity(channel->source_local_rest);

    return channel;
}

void free_animation_channel(AnimationChannel* channel) {
    if (!channel)
        return;

    if (channel->bone_name)
        free(channel->bone_name);
    if (channel->position_keys)
        free(channel->position_keys);
    if (channel->rotation_keys)
        free(channel->rotation_keys);
    if (channel->scale_keys)
        free(channel->scale_keys);

    free(channel);
}

int add_position_key(AnimationChannel* channel, float time, vec3 position) {
    if (!channel)
        return -1;

    size_t new_count = channel->position_key_count + 1;
    PositionKey* new_keys = realloc(channel->position_keys, new_count * sizeof(PositionKey));
    if (!new_keys)
        return -1;

    channel->position_keys = new_keys;
    channel->position_keys[channel->position_key_count].time = time;
    glm_vec3_copy(position, channel->position_keys[channel->position_key_count].position);
    channel->position_key_count = new_count;

    return 0;
}

int add_rotation_key(AnimationChannel* channel, float time, versor rotation) {
    if (!channel)
        return -1;

    size_t new_count = channel->rotation_key_count + 1;
    RotationKey* new_keys = realloc(channel->rotation_keys, new_count * sizeof(RotationKey));
    if (!new_keys)
        return -1;

    channel->rotation_keys = new_keys;
    channel->rotation_keys[channel->rotation_key_count].time = time;
    glm_quat_copy(rotation, channel->rotation_keys[channel->rotation_key_count].rotation);
    channel->rotation_key_count = new_count;

    return 0;
}

int add_scale_key(AnimationChannel* channel, float time, vec3 scale) {
    if (!channel)
        return -1;

    size_t new_count = channel->scale_key_count + 1;
    ScaleKey* new_keys = realloc(channel->scale_keys, new_count * sizeof(ScaleKey));
    if (!new_keys)
        return -1;

    channel->scale_keys = new_keys;
    channel->scale_keys[channel->scale_key_count].time = time;
    glm_vec3_copy(scale, channel->scale_keys[channel->scale_key_count].scale);
    channel->scale_key_count = new_count;

    return 0;
}

// ============================================================================
// Animation
// ============================================================================

Animation* create_animation(const char* name, float duration, float ticks_per_second) {
    Animation* animation = malloc(sizeof(Animation));
    if (!animation) {
        log_error("Failed to allocate memory for Animation");
        return NULL;
    }

    animation->name = safe_strdup(name);
    animation->duration = duration;
    animation->ticks_per_second = ticks_per_second > 0.0f ? ticks_per_second : 25.0f;

    animation->channels = NULL;
    animation->channel_count = 0;
    animation->events = NULL;
    animation->event_count = 0;
    animation->skeleton = NULL;

    return animation;
}

void free_animation(Animation* animation) {
    if (!animation)
        return;

    // Free all channels
    for (size_t i = 0; i < animation->channel_count; i++) {
        AnimationChannel* channel = &animation->channels[i];
        if (channel->bone_name)
            free(channel->bone_name);
        if (channel->position_keys)
            free(channel->position_keys);
        if (channel->rotation_keys)
            free(channel->rotation_keys);
        if (channel->scale_keys)
            free(channel->scale_keys);
    }

    if (animation->channels)
        free(animation->channels);

    for (size_t i = 0; i < animation->event_count; i++)
        free(animation->events[i].name);
    free(animation->events);

    if (animation->name)
        free(animation->name);

    free(animation);
}

int animation_add_event(Animation* animation, float time_ticks, const char* name) {
    if (!animation || !name)
        return -1;
    if (time_ticks < 0.0f || time_ticks > animation->duration) {
        log_error("Animation '%s': event '%s' at %.2f ticks is outside the clip (0..%.2f)",
                  animation->name, name, time_ticks, animation->duration);
        return -1;
    }

    size_t new_count = animation->event_count + 1;
    AnimationEvent* events = realloc(animation->events, new_count * sizeof(AnimationEvent));
    if (!events) {
        log_error("Failed to allocate memory for animation event");
        return -1;
    }
    animation->events = events;

    // Insert after every event at an earlier or equal time, so two events on
    // one tick keep the order they were added in.
    size_t at = animation->event_count;
    while (at > 0 && events[at - 1].time_ticks > time_ticks) {
        events[at] = events[at - 1];
        at--;
    }
    events[at].time_ticks = time_ticks;
    events[at].name = safe_strdup(name);
    animation->event_count = new_count;
    return 0;
}

int add_channel_to_animation(Animation* animation, AnimationChannel* channel) {
    if (!animation || !channel)
        return -1;

    size_t new_count = animation->channel_count + 1;
    AnimationChannel* new_channels =
        realloc(animation->channels, new_count * sizeof(AnimationChannel));
    if (!new_channels) {
        log_error("Failed to allocate memory for animation channel");
        return -1;
    }

    animation->channels = new_channels;

    // Copy channel data (shallow copy, ownership transfers)
    animation->channels[animation->channel_count] = *channel;

    // Clear the source channel to prevent double-free
    channel->bone_name = NULL;
    channel->position_keys = NULL;
    channel->rotation_keys = NULL;
    channel->scale_keys = NULL;

    animation->channel_count = new_count;

    return 0;
}

AnimationChannel* get_channel_for_bone(const Animation* animation, int bone_index) {
    if (!animation || bone_index < 0)
        return NULL;

    for (size_t i = 0; i < animation->channel_count; i++) {
        if (animation->channels[i].bone_index == bone_index)
            return &animation->channels[i];
    }

    return NULL;
}

AnimationChannel* get_channel_for_bone_name(Animation* animation, const char* bone_name) {
    if (!animation || !bone_name)
        return NULL;

    for (size_t i = 0; i < animation->channel_count; i++) {
        if (animation->channels[i].bone_name &&
            strcmp(animation->channels[i].bone_name, bone_name) == 0)
            return &animation->channels[i];
    }

    return NULL;
}

// ============================================================================
// Animation State
// ============================================================================

AnimationState* create_animation_state(Skeleton* skeleton) {
    if (!skeleton) {
        log_error("Cannot create AnimationState without skeleton");
        return NULL;
    }
    if (skeleton->bone_count > MAX_BONES) {
        // bone_matrices and prev_bone_rows are MAX_BONES wide; posing more
        // would write past them.
        log_error("Skeleton '%s' has %zu bones; the skinning path carries at most %d",
                  skeleton->name, skeleton->bone_count, MAX_BONES);
        return NULL;
    }

    // calloc: prev_bone_rows and the debug latch are read before anything
    // writes them (the first frame's latch, the first pose's dump).
    AnimationState* state = calloc(1, sizeof(AnimationState));
    if (!state) {
        log_error("Failed to allocate memory for AnimationState");
        return NULL;
    }

    state->skeleton = skeleton;

    // Initialize bone matrices to identity
    for (int i = 0; i < MAX_BONES; i++) {
        glm_mat4_identity(state->bone_matrices[i]);
    }
    state->active_bone_count = skeleton->bone_count;

    // Allocate scratch space
    state->local_transforms = malloc(skeleton->bone_count * sizeof(mat4));
    state->global_transforms = malloc(skeleton->bone_count * sizeof(mat4));

    if (!state->local_transforms || !state->global_transforms) {
        log_error("Failed to allocate scratch space for AnimationState");
        free(state->local_transforms);
        free(state->global_transforms);
        free(state);
        return NULL;
    }

    // Initialize to identity
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_identity(state->local_transforms[i]);
        glm_mat4_identity(state->global_transforms[i]);
    }

    state->springs = NULL;
    state->ik = NULL;

    // Compute initial bind pose
    compute_bind_pose_matrices(state);

    return state;
}

void free_animation_state(AnimationState* state) {
    if (!state)
        return;

    if (state->springs)
        free_spring_bone_system(state->springs);

    if (state->ik)
        free_ik_system(state->ik);
    if (state->ragdoll)
        free_ragdoll(state->ragdoll);

    if (state->local_transforms)
        free(state->local_transforms);
    if (state->global_transforms)
        free(state->global_transforms);

    free(state);
}

void animation_snapshot_prev_pose(AnimationState* state) {
    if (!state)
        return;
    // Pack each bone matrix as 3 affine rows (12 floats) — half a mat4 — so a
    // full previous-pose set fits the vertex uniform budget next to the current
    // mat4[MAX_BONES]. The bone matrices are affine, so the 4th row is implicit.
    for (size_t b = 0; b < state->active_bone_count; b++) {
        for (int r = 0; r < 3; r++) {
            float* dst = &state->prev_bone_rows[(b * 3 + (size_t)r) * 4];
            dst[0] = state->bone_matrices[b][0][r];
            dst[1] = state->bone_matrices[b][1][r];
            dst[2] = state->bone_matrices[b][2][r];
            dst[3] = state->bone_matrices[b][3][r];
        }
    }
}

// ============================================================================
// Keyframe Interpolation
// ============================================================================

// Find the two keyframes surrounding a time value for position keys
static size_t find_position_keyframe_index(const PositionKey* keys, size_t count, float time) {
    if (count < 2)
        return 0;

    // Binary search
    size_t lo = 0;
    size_t hi = count - 1;

    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (keys[mid].time <= time)
            lo = mid;
        else
            hi = mid;
    }

    return lo;
}

// Find the two keyframes surrounding a time value for rotation keys
static size_t find_rotation_keyframe_index(const RotationKey* keys, size_t count, float time) {
    if (count < 2)
        return 0;

    size_t lo = 0;
    size_t hi = count - 1;

    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (keys[mid].time <= time)
            lo = mid;
        else
            hi = mid;
    }

    return lo;
}

// Find the two keyframes surrounding a time value for scale keys
static size_t find_scale_keyframe_index(const ScaleKey* keys, size_t count, float time) {
    if (count < 2)
        return 0;

    size_t lo = 0;
    size_t hi = count - 1;

    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (keys[mid].time <= time)
            lo = mid;
        else
            hi = mid;
    }

    return lo;
}

void interpolate_position(PositionKey* keys, size_t count, float time, vec3 out) {
    if (!keys || count == 0) {
        glm_vec3_zero(out);
        return;
    }

    if (count == 1) {
        glm_vec3_copy(keys[0].position, out);
        return;
    }

    // Clamp to valid range
    if (time <= keys[0].time) {
        glm_vec3_copy(keys[0].position, out);
        return;
    }
    if (time >= keys[count - 1].time) {
        glm_vec3_copy(keys[count - 1].position, out);
        return;
    }

    size_t idx = find_position_keyframe_index(keys, count, time);

    // Interpolate between keys[idx] and keys[idx+1]
    float t1 = keys[idx].time;
    float t2 = keys[idx + 1].time;
    if (t2 <= t1) {
        glm_vec3_copy(keys[idx].position, out);
        return;
    }
    float factor = (time - t1) / (t2 - t1);

    glm_vec3_lerp(keys[idx].position, keys[idx + 1].position, factor, out);
}

void interpolate_rotation(RotationKey* keys, size_t count, float time, versor out) {
    if (!keys || count == 0) {
        glm_quat_identity(out);
        return;
    }

    if (count == 1) {
        glm_quat_copy(keys[0].rotation, out);
        return;
    }

    // Clamp to valid range
    if (time <= keys[0].time) {
        glm_quat_copy(keys[0].rotation, out);
        return;
    }
    if (time >= keys[count - 1].time) {
        glm_quat_copy(keys[count - 1].rotation, out);
        return;
    }

    size_t i = find_rotation_keyframe_index(keys, count, time);

    // Interpolate between keys[i] and keys[i+1] using SLERP
    float t1 = keys[i].time;
    float t2 = keys[i + 1].time;
    if (t2 <= t1) {
        glm_quat_copy(keys[i].rotation, out);
        return;
    }
    float factor = (time - t1) / (t2 - t1);

    glm_quat_slerp(keys[i].rotation, keys[i + 1].rotation, factor, out);
}

void interpolate_scale(ScaleKey* keys, size_t count, float time, vec3 out) {
    if (!keys || count == 0) {
        glm_vec3_one(out);
        return;
    }

    if (count == 1) {
        glm_vec3_copy(keys[0].scale, out);
        return;
    }

    // Clamp to valid range
    if (time <= keys[0].time) {
        glm_vec3_copy(keys[0].scale, out);
        return;
    }
    if (time >= keys[count - 1].time) {
        glm_vec3_copy(keys[count - 1].scale, out);
        return;
    }

    size_t i = find_scale_keyframe_index(keys, count, time);

    // Interpolate between keys[i] and keys[i+1]
    float t1 = keys[i].time;
    float t2 = keys[i + 1].time;
    if (t2 <= t1) {
        glm_vec3_copy(keys[i].scale, out);
        return;
    }
    float factor = (time - t1) / (t2 - t1);

    glm_vec3_lerp(keys[i].scale, keys[i + 1].scale, factor, out);
}

// ============================================================================
// Bone Matrix Computation
// ============================================================================

/*
 * One-shot diagnostic: per-bone bind vs animated global positions, drift,
 * and skinning-matrix column scale (flags scale corruption)
 */
static void print_bone_drift_debug(AnimationState* state, const Pose* pose) {
    Skeleton* skeleton = state->skeleton;

    printf("\n========== ANIMATION DEBUG ==========\n");
    printf("Format: [idx] name | ch=a clip drives it | bind global pos -> animated global pos "
           "| drift | bone matrix scale\n\n");

    mat4* bind_globals = malloc(skeleton->bone_count * sizeof(mat4));
    if (bind_globals) {
        skeleton_compute_bind_globals(skeleton, bind_globals);

        for (size_t i = 0; i < skeleton->bone_count; i++) {
            bool channel = pose->driven[i] != 0;

            vec3 bind_pos, anim_pos;
            glm_vec3_copy(bind_globals[i][3], bind_pos);
            glm_vec3_copy(state->global_transforms[i][3], anim_pos);
            float drift = glm_vec3_distance(bind_pos, anim_pos);

            // Column norms of the final skinning matrix reveal scale errors
            float sx = glm_vec3_norm(state->bone_matrices[i][0]);
            float sy = glm_vec3_norm(state->bone_matrices[i][1]);
            float sz = glm_vec3_norm(state->bone_matrices[i][2]);

            printf("[%3zu] %-24s | ch=%d | (%6.2f,%6.2f,%6.2f) -> (%6.2f,%6.2f,%6.2f) | "
                   "drift=%6.2f | scale=(%.2f,%.2f,%.2f)%s\n",
                   i, skeleton->bones[i].name, channel ? 1 : 0, bind_pos[0], bind_pos[1],
                   bind_pos[2], anim_pos[0], anim_pos[1], anim_pos[2], drift, sx, sy, sz,
                   (sx < 0.5f || sx > 2.0f || sy < 0.5f || sy > 2.0f || sz < 0.5f || sz > 2.0f)
                       ? "  <<< SCALE"
                       : "");
        }
        free(bind_globals);
    }

    printf("\n========== END DEBUG ==========\n\n");
}

// ============================================================================
// Pose: sample, blend, apply
// ============================================================================

// The ONE place a local matrix is built from TRS: T * R * S in this order and
// through these calls, so the hierarchy the sampler accumulates in scratch and
// the one the apply writes into the state are the same bits.
static void bone_local_from_trs(const BoneTransform* bt, mat4 out) {
    mat4 trans, rotation, scaling;
    glm_translate_make(trans, (float*)bt->position);
    glm_quat_mat4((float*)bt->rotation, rotation);
    glm_scale_make(scaling, (float*)bt->scale);

    mat4 temp;
    glm_mat4_mul(trans, rotation, temp);
    glm_mat4_mul(temp, scaling, out);
}

// A driven bone's local from its TRS; an undriven one's bind matrix verbatim.
static void pose_local(const Skeleton* skeleton, const Pose* pose, size_t i, mat4 out) {
    if (pose->driven[i])
        bone_local_from_trs(&pose->bones[i], out);
    else
        glm_mat4_copy((vec4*)skeleton->bones[i].local_transform, out);
}

// global[i] = global[parent] * local[i], parent-first. The bounds guard is the
// same one the state's accumulation has always had.
static void accumulate_global(const Bone* bone, size_t i, size_t bone_count, mat4* locals,
                              mat4* globals) {
    if (bone->parent_index < 0) {
        glm_mat4_copy(locals[i], globals[i]);
    } else if ((size_t)bone->parent_index < bone_count) {
        glm_mat4_mul(globals[bone->parent_index], locals[i], globals[i]);
    } else {
        glm_mat4_copy(locals[i], globals[i]);
    }
}

static void bind_transform(const Bone* bone, BoneTransform* out) {
    vec4 t;
    mat4 r;
    glm_decompose((vec4*)bone->local_transform, t, r, out->scale);
    glm_vec3_copy(t, out->position);
    glm_mat4_quat(r, out->rotation);
}

void pose_bind(const Skeleton* skeleton, Pose* out) {
    if (!skeleton || !out)
        return;
    out->skeleton = skeleton;
    out->bone_count = skeleton->bone_count;
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        bind_transform(&skeleton->bones[i], &out->bones[i]);
        out->driven[i] = 0;
    }
}

void animation_sample_pose(const Animation* anim, const Skeleton* skeleton, float time, Pose* out) {
    if (!skeleton || !out)
        return;
    if (!anim) {
        pose_bind(skeleton, out);
        return;
    }
    if (anim->skeleton && anim->skeleton != skeleton) {
        log_error("Animation '%s' is bound to skeleton '%s', not '%s'; sampling the bind pose",
                  anim->name, anim->skeleton->name, skeleton->name);
        pose_bind(skeleton, out);
        return;
    }

    out->skeleton = skeleton;
    out->bone_count = skeleton->bone_count;

    // Check if any channel uses global retargeting
    bool has_global_retarget = false;
    for (size_t c = 0; c < anim->channel_count; c++) {
        if (anim->channels[c].use_global_retarget) {
            has_global_retarget = true;
            break;
        }
    }

    // Scratch, per call: the source skeleton's global rotations (indexed by
    // source_bone_index) and this clip's own posed hierarchy, which the global
    // retarget derives each child's local against. A second clip sampled next
    // starts from nothing of this one.
    versor source_globals_animated[MAX_BONES]; // Accumulated animated rotations
    versor source_globals_rest[MAX_BONES];     // Accumulated rest pose rotations
    bool source_global_computed[MAX_BONES];
    mat4 locals[MAX_BONES];
    mat4 globals[MAX_BONES];

    if (has_global_retarget) {
        // PASS 0: Compute source skeleton global rotations (both animated and rest)
        // Process in order of source_bone_index (source skeleton is parent-first)
        memset(source_global_computed, 0, sizeof(source_global_computed));

        for (int src_idx = 0; src_idx < MAX_BONES; src_idx++) {
            // Find channel that has this source_bone_index
            const AnimationChannel* channel = NULL;
            for (size_t c = 0; c < anim->channel_count; c++) {
                if (anim->channels[c].use_global_retarget &&
                    anim->channels[c].source_bone_index == src_idx) {
                    channel = &anim->channels[c];
                    break;
                }
            }
            if (!channel)
                continue;

            // Interpolate keyframe rotation
            versor keyframe = {0.0f, 0.0f, 0.0f, 1.0f};
            interpolate_rotation(channel->rotation_keys, channel->rotation_key_count, time,
                                 keyframe);

            // Compute source global ANIMATED:
            // source_global_animated = source_parent_global_animated * keyframe
            int src_parent = channel->source_parent_bone_index;
            if (src_parent < 0 || !source_global_computed[src_parent]) {
                // Root bone or parent not animated
                glm_quat_copy(keyframe, source_globals_animated[src_idx]);
                glm_quat_copy((float*)channel->source_local_rest, source_globals_rest[src_idx]);
            } else {
                // Child bone: accumulate through parent
                glm_quat_mul(source_globals_animated[src_parent], keyframe,
                             source_globals_animated[src_idx]);
                glm_quat_mul(source_globals_rest[src_parent], (float*)channel->source_local_rest,
                             source_globals_rest[src_idx]);
            }
            glm_quat_normalize(source_globals_animated[src_idx]);
            glm_quat_normalize(source_globals_rest[src_idx]);
            source_global_computed[src_idx] = true;
        }
    }

    // PASS 1: one bone's TRS at a time. Under global retargeting the hierarchy
    // is accumulated alongside, because deriving a child's local needs the
    // parent's animated global from THIS clip.
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        const Bone* bone = &skeleton->bones[i];
        const AnimationChannel* channel = get_channel_for_bone(anim, (int)i);
        BoneTransform* bt = &out->bones[i];

        if (channel && channel->use_global_retarget && channel->source_bone_index >= 0 &&
            source_global_computed[channel->source_bone_index]) {
            // GLOBAL-SPACE RETARGETING WITH REST POSE COMPENSATION
            int src_idx = channel->source_bone_index;

            // Step 1: Extract MOTION from source skeleton in WORLD frame
            // motion = source_global_animated * inv(source_global_rest)
            // This is the world-space rotation taking the bone from rest to animated.
            // World frame is used (not the bone's rest-local frame) because matched
            // bones in different rigs share world orientation at rest (both characters
            // stand upright facing forward) but do not share local axis conventions.
            versor source_rest_inv;
            glm_quat_inv(source_globals_rest[src_idx], source_rest_inv);
            versor motion;
            glm_quat_mul(source_globals_animated[src_idx], source_rest_inv, motion);
            glm_quat_normalize(motion);

            // Step 2: Get target bone's global rest rotation
            // target_global_rest = inv(inverse_bind_pose) rotation
            mat4 target_global_rest_mat;
            glm_mat4_inv((vec4*)bone->inverse_bind_pose, target_global_rest_mat);
            versor target_global_rest;
            glm_mat4_quat(target_global_rest_mat, target_global_rest);
            glm_quat_normalize(target_global_rest);

            // Step 3: Apply world-space motion to target rest pose
            // target_global_animated = motion * target_global_rest
            versor target_global_animated;
            glm_quat_mul(motion, target_global_rest, target_global_animated);
            glm_quat_normalize(target_global_animated);

            // Step 4: Get target parent's current global rotation (from animated transform)
            versor target_parent_global_rot;
            glm_quat_identity(target_parent_global_rot);
            if (bone->parent_index >= 0 && (size_t)bone->parent_index < skeleton->bone_count) {
                glm_mat4_quat(globals[bone->parent_index], target_parent_global_rot);
            }

            // Step 5: Derive target local rotation
            // target_local = inv(target_parent_global) * target_global_animated
            versor target_parent_inv;
            glm_quat_inv(target_parent_global_rot, target_parent_inv);
            glm_quat_mul(target_parent_inv, target_global_animated, bt->rotation);
            glm_quat_normalize(bt->rotation);

            // Position from bind pose (retargeted animations use bind pose position)
            glm_vec3_copy((float*)bone->local_transform[3], bt->position);

            glm_vec3_one(bt->scale);
            if (channel->scale_key_count > 0) {
                interpolate_scale(channel->scale_keys, channel->scale_key_count, time, bt->scale);
            }
            out->driven[i] = 1;

        } else if (channel) {
            // ORIGINAL LOCAL-DELTA RETARGETING (fallback)
            glm_vec3_zero(bt->position);
            glm_vec3_one(bt->scale);
            glm_quat_identity(bt->rotation);

            interpolate_rotation(channel->rotation_keys, channel->rotation_key_count, time,
                                 bt->rotation);
            interpolate_scale(channel->scale_keys, channel->scale_key_count, time, bt->scale);

            // Apply retargeting correction if needed (local delta approach)
            if (channel->needs_retargeting && !channel->use_global_retarget) {
                versor result;
                glm_quat_mul(bt->rotation, (float*)channel->rotation_delta, result);
                glm_quat_copy(result, bt->rotation);
            }

            // For retargeted animations, use bind pose position
            if (channel->needs_retargeting) {
                glm_vec3_copy((float*)bone->local_transform[3], bt->position);
            } else {
                interpolate_position(channel->position_keys, channel->position_key_count, time,
                                     bt->position);
            }
            out->driven[i] = 1;

        } else {
            // No channel: the bind local, and the flag that makes the apply
            // copy its matrix rather than rebuild it from these numbers.
            bind_transform(bone, bt);
            out->driven[i] = 0;
        }

        if (has_global_retarget) {
            pose_local(skeleton, out, i, locals[i]);
            accumulate_global(bone, i, skeleton->bone_count, locals, globals);
        }
    }
}

// ============================================================================
// What a clip's feet imply about the ground
// ============================================================================

// A foot in the lower part of its own vertical range is standing on something. HALF that
// range, which is wide on purpose: what makes the answer robust is the median below, not
// a tight window, and a tight one is actively harmful. At 0.15 of the lift a real stance
// split into three separate runs on a rig whose hip translation the retarget had replaced
// by its bind position -- the foot hovers rather than resting flat, so it crosses back out
// of a narrow band twice per stance and reads as a clip with no stance at all.
#define STRIDE_BAND     0.5f
#define STRIDE_MIN_LIFT 1e-4f
// A loop is sampled at this rate. High enough that a short stance is several samples, low
// enough that a long clip stays cheap; the bounds keep both true for a clip of any length.
#define STRIDE_HZ        60.0f
#define STRIDE_MIN_SAMPS 12
#define STRIDE_MAX_SAMPS 512
// Fewer in-band samples than this and there is no stance to take a median over.
#define STRIDE_MIN_DWELL 4

// What one foot says about the ground.
typedef struct FootDwell {
    int samples; // how many of the loop's samples it spent down, which is its weight
    vec3 speed;  // the ground's velocity past it while it was, model units per second
    float lift;  // its total vertical travel over the loop
} FootDwell;

// The middle value of `v[0..n)`, which sorts in place. A median and not a mean: the
// samples handed to it are "the foot is low", and a few of those are the moments either
// side of a stance where it is already moving. A mean lets those drag the answer; a
// median does not notice them.
static float median_of(float* v, int n) {
    for (int i = 1; i < n; i++) {
        const float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

bool animation_stride_speed(const Animation* clip, const Skeleton* skeleton, const int ankle[2],
                            const int toe[2], float* out_speed, vec3 out_dir) {
    if (!clip || !skeleton || !ankle || !toe || !out_speed)
        return false;
    if (clip->duration <= 0.0f || clip->ticks_per_second <= 0.0f)
        return false;
    for (int f = 0; f < 2; f++) {
        if (ankle[f] < 0 || (size_t)ankle[f] >= skeleton->bone_count || toe[f] < 0 ||
            (size_t)toe[f] >= skeleton->bone_count)
            return false;
    }

    const float seconds = clip->duration / clip->ticks_per_second;
    int n = (int)(seconds * STRIDE_HZ + 0.5f);
    if (n < STRIDE_MIN_SAMPS)
        n = STRIDE_MIN_SAMPS;
    if (n > STRIDE_MAX_SAMPS)
        n = STRIDE_MAX_SAMPS;

    Pose* pose = malloc(sizeof(Pose));
    mat4* locals = malloc(sizeof(mat4) * MAX_BONES);
    mat4* globals = malloc(sizeof(mat4) * MAX_BONES);
    vec3* ank = malloc(sizeof(vec3) * (size_t)n * 2);
    vec3* tip = malloc(sizeof(vec3) * (size_t)n * 2);
    bool ok = pose && locals && globals && ank && tip;

    // One loop, sampled evenly. The clip's last key IS its first, so the sample after
    // n-1 is sample 0 and the central differences below may wrap freely.
    for (int i = 0; ok && i < n; i++) {
        animation_sample_pose(clip, skeleton, clip->duration * (float)i / (float)n, pose);
        for (size_t b = 0; b < skeleton->bone_count; b++) {
            pose_local(skeleton, pose, b, locals[b]);
            accumulate_global(&skeleton->bones[b], b, skeleton->bone_count, locals, globals);
        }
        for (int f = 0; f < 2; f++) {
            glm_vec3_copy(globals[ankle[f]][3], ank[f * n + i]);
            glm_vec3_copy(globals[toe[f]][3], tip[f * n + i]);
        }
    }

    FootDwell dwell[2] = {{0, {0.0f, 0.0f, 0.0f}, 0.0f}, {0, {0.0f, 0.0f, 0.0f}, 0.0f}};
    float* vx = malloc(sizeof(float) * (size_t)n);
    float* vz = malloc(sizeof(float) * (size_t)n);
    ok = ok && vx && vz;
    // Both feet always. Nothing here refuses -- a foot with no lift or too few low samples
    // leaves its speed at zero and the agreement test below is what turns that into a
    // refusal. A loop that stopped at the first failure would leave the other foot
    // unmeasured, and the trace then reads as though the foot nobody looked at were the
    // one at fault, which cost an hour once.
    for (int f = 0; ok && f < 2; f++) {
        float low = 1e30f, high = -1e30f;
        for (int i = 0; i < n; i++) {
            const float y = ank[f * n + i][1];
            low = y < low ? y : low;
            high = y > high ? y : high;
        }
        dwell[f].lift = high - low;
        if (high - low < STRIDE_MIN_LIFT)
            continue; // the foot never leaves the ground, so nothing marks a stance

        // The toe's horizontal velocity at every sample the ankle spent low, by central
        // difference around the loop. The samples need NOT be contiguous and nothing here
        // assumes they are: that is the whole reason this is a median and not a fit over a
        // window, since a window has to be found and a found window can be found wrong.
        for (int i = 0; i < n; i++) {
            if (ank[f * n + i][1] > low + STRIDE_BAND * (high - low))
                continue;
            const vec3* p = &tip[f * n];
            const int a = (i + n - 1) % n, b = (i + 1) % n;
            vx[dwell[f].samples] = (p[b][0] - p[a][0]) * STRIDE_HZ * 0.5f;
            vz[dwell[f].samples] = (p[b][2] - p[a][2]) * STRIDE_HZ * 0.5f;
            dwell[f].samples++;
        }
        if (dwell[f].samples < STRIDE_MIN_DWELL)
            continue;
        dwell[f].speed[0] = median_of(vx, dwell[f].samples);
        dwell[f].speed[2] = median_of(vz, dwell[f].samples);
    }
    free(vx);
    free(vz);

    // Every number below is an aggregate over two feet, and an aggregate cannot say which
    // foot produced it. CETRA_STRIDE_TRACE=1 prints the per-foot dwell and speed to
    // stderr, which is what a disagreement with an independent measurement has to be
    // resolved against.
    if (getenv("CETRA_STRIDE_TRACE")) {
        for (int f = 0; f < 2; f++)
            fprintf(stderr,
                    "stride-trace %s foot %d bones %s/%s samples %d lift %.4f down %d "
                    "velocity %.4f %.4f speed %.6f\n",
                    clip->name ? clip->name : "?", f, skeleton->bones[ankle[f]].name,
                    skeleton->bones[toe[f]].name, n, (double)dwell[f].lift, dwell[f].samples,
                    (double)dwell[f].speed[0], (double)dwell[f].speed[2],
                    (double)glm_vec3_norm(dwell[f].speed));
    }

    float speed = 0.0f;
    vec3 pooled = {0.0f, 0.0f, 0.0f};
    if (ok) {
        // The two feet must AGREE, in direction and in magnitude. Two independent
        // measurements of one quantity: a clip whose feet disagree has no single ground
        // speed to report. This is also what refuses a straight-leg pendulum, and twice
        // over -- its feet are lowest at the same instant and swinging OPPOSITE ways, so
        // they disagree in direction, and over a whole loop each one's low samples carry
        // both directions and median to nothing.
        const float m0 = glm_vec3_norm(dwell[0].speed), m1 = glm_vec3_norm(dwell[1].speed);
        // The tolerance is wide because a stylised walk is allowed to be asymmetric: one
        // foot dragging is a property of the clip and not a broken measurement.
        ok = m0 > 1e-4f && m1 > 1e-4f && fabsf(m0 - m1) < 0.35f * 0.5f * (m0 + m1);
        if (ok) {
            vec3 u0, u1;
            glm_vec3_scale(dwell[0].speed, 1.0f / m0, u0);
            glm_vec3_scale(dwell[1].speed, 1.0f / m1, u1);
            ok = glm_vec3_dot(u0, u1) > 0.9f;
        }
    }
    if (ok) {
        // ONE answer over both feet rather than the mean of two, weighted by how long each
        // foot was down -- a foot that stands for more of the loop pins the body over more
        // of it and says more about its speed. On an asymmetric clip the difference is
        // real; a plain mean gives a brief, noisy stance the same vote as a long one.
        vec3 a, b;
        glm_vec3_scale(dwell[0].speed, (float)dwell[0].samples, a);
        glm_vec3_scale(dwell[1].speed, (float)dwell[1].samples, b);
        glm_vec3_add(a, b, pooled);
        glm_vec3_scale(pooled, 1.0f / (float)(dwell[0].samples + dwell[1].samples), pooled);
        speed = glm_vec3_norm(pooled);
        ok = speed > 1e-4f;
    }

    if (ok) {
        *out_speed = speed;
        if (out_dir) {
            // The body goes the way the stance feet do not.
            glm_vec3_scale(pooled, -1.0f / speed, out_dir);
        }
    }

    free(pose);
    free(locals);
    free(globals);
    free(ank);
    free(tip);
    return ok;
}

static void blend_bone(const BoneTransform* a, const BoneTransform* b, float t,
                       BoneTransform* out) {
    glm_vec3_lerp((float*)a->position, (float*)b->position, t, out->position);
    glm_quat_nlerp((float*)a->rotation, (float*)b->rotation, t, out->rotation);
    glm_vec3_lerp((float*)a->scale, (float*)b->scale, t, out->scale);
}

void pose_blend(const Pose* a, const Pose* b, float t, Pose* out) {
    if (!a || !b || !out)
        return;
    if (a->skeleton != b->skeleton) {
        log_error("pose_blend: poses for different skeletons ('%s', '%s')",
                  a->skeleton ? a->skeleton->name : "none",
                  b->skeleton ? b->skeleton->name : "none");
        return;
    }
    if (t <= 0.0f) {
        if (out != a)
            *out = *a;
        return;
    }
    if (t >= 1.0f) {
        if (out != b)
            *out = *b;
        return;
    }

    size_t n = a->bone_count;
    for (size_t i = 0; i < n; i++) {
        blend_bone(&a->bones[i], &b->bones[i], t, &out->bones[i]);
        out->driven[i] = a->driven[i] || b->driven[i];
    }
    out->skeleton = a->skeleton;
    out->bone_count = n;
}

void pose_blend_masked(const Pose* base, const Pose* over, const float* bone_weights, Pose* out) {
    if (!base || !over || !bone_weights || !out)
        return;
    if (base->skeleton != over->skeleton) {
        log_error("pose_blend_masked: poses for different skeletons ('%s', '%s')",
                  base->skeleton ? base->skeleton->name : "none",
                  over->skeleton ? over->skeleton->name : "none");
        return;
    }

    size_t n = base->bone_count;
    for (size_t i = 0; i < n; i++) {
        float w = bone_weights[i];
        if (w <= 0.0f) {
            if (out != base) {
                out->bones[i] = base->bones[i];
                out->driven[i] = base->driven[i];
            }
        } else if (w >= 1.0f) {
            if (out != over) {
                out->bones[i] = over->bones[i];
                out->driven[i] = over->driven[i];
            }
        } else {
            blend_bone(&base->bones[i], &over->bones[i], w, &out->bones[i]);
            out->driven[i] = base->driven[i] || over->driven[i];
        }
    }
    out->skeleton = base->skeleton;
    out->bone_count = n;
}

void animation_state_apply_pose(AnimationState* state, const Pose* pose, float delta_time) {
    if (!state || !state->skeleton || !pose)
        return;

    Skeleton* skeleton = state->skeleton;
    if (pose->skeleton != skeleton) {
        log_error("Pose for skeleton '%s' applied to a state on '%s'; ignored",
                  pose->skeleton ? pose->skeleton->name : "none", skeleton->name);
        return;
    }

    // Locals from the pose, globals accumulated parent-first
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        pose_local(skeleton, pose, i, state->local_transforms[i]);
        accumulate_global(&skeleton->bones[i], i, skeleton->bone_count, state->local_transforms,
                          state->global_transforms);
    }

    // Spring-bone secondary motion: simulate un-animated chains (scabbard,
    // hair) on top of the animated pose before skinning matrices are built
    if (state->springs) {
        spring_bone_update(state->springs, state->local_transforms, state->global_transforms,
                           delta_time);
    }

    // Two-bone IK: plant a foot on ground the clip knew nothing about. AFTER the
    // springs, because their pass re-accumulates every unsimulated bone from its
    // parent and would erase a solve written before it (see ik.h).
    if (state->ik)
        ik_solve(state->ik, state->global_transforms, delta_time);

    // The ragdoll, LAST and for the opposite reason to the two above: they
    // correct a pose the clip produced, and this replaces it. A foot planted on
    // ground a falling body is no longer standing on is not a correction worth
    // keeping, so nothing above this needs to run first -- it needs to have its
    // result overwritten. A no-op while inactive.
    if (state->ragdoll)
        ragdoll_apply(state->ragdoll, state->global_transforms, skeleton->bone_count);

    // PASS 2: Compute final bone matrices (global * inverse bind pose)
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_mul(state->global_transforms[i], skeleton->bones[i].inverse_bind_pose,
                     state->bone_matrices[i]);
    }

    // One-shot diagnostic dump of the first driven pose (opt-in).
    if (state->debug_pose_dump && !state->debug_pose_dumped) {
        bool driven = false;
        for (size_t i = 0; i < skeleton->bone_count && !driven; i++)
            driven = pose->driven[i] != 0;
        if (driven) {
            print_bone_drift_debug(state, pose);
            state->debug_pose_dumped = true;
        }
    }

    state->active_bone_count = skeleton->bone_count;
}

void compute_bind_pose_matrices(AnimationState* state) {
    if (!state || !state->skeleton)
        return;

    Skeleton* skeleton = state->skeleton;

    // Bind pose: locals from the skeleton, globals accumulated parent-first
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_copy(skeleton->bones[i].local_transform, state->local_transforms[i]);
    }
    skeleton_compute_bind_globals(skeleton, state->global_transforms);

    // Final bone matrices = global * inverse bind pose
    // For bind pose, this should result in identity matrices
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_mul(state->global_transforms[i], skeleton->bones[i].inverse_bind_pose,
                     state->bone_matrices[i]);
    }

    state->active_bone_count = skeleton->bone_count;
}

// ============================================================================
// Debug Output
// ============================================================================

void print_skeleton(const Skeleton* skeleton) {
    if (!skeleton) {
        printf("Skeleton: NULL\n");
        return;
    }

    printf("Skeleton: %s (%zu bones)\n", skeleton->name ? skeleton->name : "(unnamed)",
           skeleton->bone_count);

    for (size_t i = 0; i < skeleton->bone_count; i++) {
        const Bone* bone = &skeleton->bones[i];
        printf("  [%zu] %s (parent: %d)\n", i, bone->name ? bone->name : "(unnamed)",
               bone->parent_index);
    }
}

void print_animation(const Animation* animation) {
    if (!animation) {
        printf("Animation: NULL\n");
        return;
    }

    printf("Animation: %s\n", animation->name ? animation->name : "(unnamed)");
    printf("  Duration: %.2f ticks (%.2f ticks/sec = %.2f sec)\n", animation->duration,
           animation->ticks_per_second, animation->duration / animation->ticks_per_second);
    printf("  Channels: %zu\n", animation->channel_count);

    for (size_t i = 0; i < animation->channel_count; i++) {
        const AnimationChannel* ch = &animation->channels[i];
        printf("    [%zu] Bone %d (%s): %zu pos, %zu rot, %zu scale keys\n", i, ch->bone_index,
               ch->bone_name ? ch->bone_name : "(unnamed)", ch->position_key_count,
               ch->rotation_key_count, ch->scale_key_count);
    }
}

void print_animation_state(const AnimationState* state) {
    if (!state) {
        printf("AnimationState: NULL\n");
        return;
    }

    printf("AnimationState:\n");
    printf("  Skeleton: %s\n", state->skeleton
                                   ? (state->skeleton->name ? state->skeleton->name : "(unnamed)")
                                   : "NULL");
    printf("  Active bones: %zu\n", state->active_bone_count);
}
