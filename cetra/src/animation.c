#include "animation.h"
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

    skeleton->name = name ? strdup(name) : NULL;
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

    bone->name = strdup(name);
    bone->parent_index = parent_index;
    glm_mat4_copy(inverse_bind_pose, bone->inverse_bind_pose);
    glm_mat4_copy(local_transform, bone->local_transform);

    skeleton->bone_count = new_count;

    // Add to hash map for name lookup
    BoneIndexEntry* entry = malloc(sizeof(BoneIndexEntry));
    if (entry) {
        entry->name = strdup(name);
        entry->index = bone_index;
        HASH_ADD_KEYPTR(hh, skeleton->bone_map, entry->name, strlen(entry->name), entry);
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
    channel->bone_name = bone_name ? strdup(bone_name) : NULL;

    channel->position_keys = NULL;
    channel->position_key_count = 0;

    channel->rotation_keys = NULL;
    channel->rotation_key_count = 0;

    channel->scale_keys = NULL;
    channel->scale_key_count = 0;

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

    animation->name = name ? strdup(name) : NULL;
    animation->duration = duration;
    animation->ticks_per_second = ticks_per_second > 0.0f ? ticks_per_second : 25.0f;

    animation->channels = NULL;
    animation->channel_count = 0;
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

    if (animation->name)
        free(animation->name);

    free(animation);
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

    AnimationState* state = malloc(sizeof(AnimationState));
    if (!state) {
        log_error("Failed to allocate memory for AnimationState");
        return NULL;
    }

    state->current_animation = NULL;
    state->skeleton = skeleton;

    state->current_time = 0.0f;
    state->speed = 1.0f;
    state->looping = true;
    state->playing = false;

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

    // Compute initial bind pose
    compute_bind_pose_matrices(state);

    return state;
}

void free_animation_state(AnimationState* state) {
    if (!state)
        return;

    if (state->local_transforms)
        free(state->local_transforms);
    if (state->global_transforms)
        free(state->global_transforms);

    free(state);
}

void set_animation(AnimationState* state, Animation* animation) {
    if (!state)
        return;

    state->current_animation = animation;
    state->current_time = 0.0f;
}

void play_animation(AnimationState* state) {
    if (state)
        state->playing = true;
}

void pause_animation(AnimationState* state) {
    if (state)
        state->playing = false;
}

void stop_animation(AnimationState* state) {
    if (!state)
        return;

    state->playing = false;
    state->current_time = 0.0f;
    compute_bind_pose_matrices(state);
}

void reset_animation(AnimationState* state) {
    if (!state)
        return;

    state->current_time = 0.0f;
    if (state->current_animation) {
        compute_bone_matrices(state);
    } else {
        compute_bind_pose_matrices(state);
    }
}

void update_animation(AnimationState* state, float delta_time) {
    if (!state || !state->playing || !state->current_animation)
        return;

    const Animation* anim = state->current_animation;

    // Advance time
    float ticks_delta = delta_time * anim->ticks_per_second * state->speed;
    state->current_time += ticks_delta;

    // Handle looping/end
    if (state->looping) {
        if (anim->duration > 0.0f) {
            state->current_time = fmodf(state->current_time, anim->duration);
            if (state->current_time < 0.0f)
                state->current_time += anim->duration;
        }
    } else {
        if (state->current_time >= anim->duration) {
            state->current_time = anim->duration;
            state->playing = false;
        } else if (state->current_time < 0.0f) {
            state->current_time = 0.0f;
            state->playing = false;
        }
    }

    // Recompute bone matrices
    compute_bone_matrices(state);
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
    float factor = (time - t1) / (t2 - t1);

    glm_vec3_lerp(keys[i].scale, keys[i + 1].scale, factor, out);
}

// ============================================================================
// Bone Matrix Computation
// ============================================================================

void compute_bone_matrices(AnimationState* state) {
    if (!state || !state->skeleton)
        return;

    Skeleton* skeleton = state->skeleton;
    const Animation* anim = state->current_animation;
    float time = state->current_time;

    // Step 1: Compute local transforms from keyframes (or use bind pose)
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        AnimationChannel* channel = anim ? get_channel_for_bone(anim, (int)i) : NULL;

        if (channel) {
            vec3 pos = {0.0f, 0.0f, 0.0f};
            vec3 scale = {1.0f, 1.0f, 1.0f};
            versor rot = {0.0f, 0.0f, 0.0f, 1.0f};

            interpolate_position(channel->position_keys, channel->position_key_count, time, pos);
            interpolate_rotation(channel->rotation_keys, channel->rotation_key_count, time, rot);
            interpolate_scale(channel->scale_keys, channel->scale_key_count, time, scale);

            // Build local transform: T * R * S
            mat4 trans, rotation, scaling;
            glm_translate_make(trans, pos);
            glm_quat_mat4(rot, rotation);
            glm_scale_make(scaling, scale);

            // local = T * R * S
            mat4 temp;
            glm_mat4_mul(trans, rotation, temp);
            glm_mat4_mul(temp, scaling, state->local_transforms[i]);
        } else {
            // Use bind pose local transform
            glm_mat4_copy(bone->local_transform, state->local_transforms[i]);
        }
    }

    // Step 2: Compute global transforms (parent-first iteration)
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];

        if (bone->parent_index < 0) {
            // Root bone - global = local
            glm_mat4_copy(state->local_transforms[i], state->global_transforms[i]);
        } else {
            // global = parent_global * local
            glm_mat4_mul(state->global_transforms[bone->parent_index], state->local_transforms[i],
                         state->global_transforms[i]);
        }
    }

    // Step 3: Compute final bone matrices (global * inverse bind pose)
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        glm_mat4_mul(state->global_transforms[i], skeleton->bones[i].inverse_bind_pose,
                     state->bone_matrices[i]);
    }

    state->active_bone_count = skeleton->bone_count;
}

void compute_bind_pose_matrices(AnimationState* state) {
    if (!state || !state->skeleton)
        return;

    Skeleton* skeleton = state->skeleton;

    // Compute global transforms from bind pose local transforms
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        glm_mat4_copy(bone->local_transform, state->local_transforms[i]);

        if (bone->parent_index < 0) {
            glm_mat4_copy(state->local_transforms[i], state->global_transforms[i]);
        } else {
            glm_mat4_mul(state->global_transforms[bone->parent_index], state->local_transforms[i],
                         state->global_transforms[i]);
        }
    }

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
    printf("  Animation: %s\n",
           state->current_animation
               ? (state->current_animation->name ? state->current_animation->name : "(unnamed)")
               : "NULL");
    printf("  Time: %.2f, Speed: %.2f, %s, %s\n", state->current_time, state->speed,
           state->playing ? "PLAYING" : "PAUSED", state->looping ? "LOOPING" : "ONCE");
    printf("  Active bones: %zu\n", state->active_bone_count);
}
