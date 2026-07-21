#include "springbone.h"
#include "ext/log.h"

#include <stdlib.h>
#include <string.h>

// Fixed simulation substep so behavior is framerate independent. The dt clamp
// bounds the accumulator so at most SPRING_MAX_SUBSTEPS run per frame.
#define SPRING_STEP         (1.0f / 120.0f)
#define SPRING_MAX_SUBSTEPS 4
#define SPRING_MAX_DT       (SPRING_MAX_SUBSTEPS * SPRING_STEP)

SpringBoneParams spring_bone_default_params(void) {
    SpringBoneParams params = {
        .stiffness = 0.25f,
        .damping = 0.25f,
        .gravity = 4.0f,
        .max_stretch = 0.0f,
        .max_angle_deg = 45.0f,
        .teleport_distance = 0.5f,
    };
    return params;
}

SpringBoneSystem* create_spring_bone_system(Skeleton* skeleton) {
    if (!skeleton || skeleton->bone_count == 0 || skeleton->bone_count > MAX_BONES) {
        log_error("Cannot create SpringBoneSystem: invalid skeleton");
        return NULL;
    }

    SpringBoneSystem* system = malloc(sizeof(SpringBoneSystem));
    if (!system) {
        log_error("Failed to allocate SpringBoneSystem");
        return NULL;
    }

    system->skeleton = skeleton;
    system->joints = NULL;
    system->joint_count = 0;
    system->params = spring_bone_default_params();
    for (int i = 0; i < MAX_BONES; i++) {
        system->bone_to_joint[i] = -1;
    }
    system->enabled = true;
    system->needs_reset = true;
    system->time_accum = 0.0f;

    return system;
}

void free_spring_bone_system(SpringBoneSystem* system) {
    if (!system)
        return;

    free(system->joints);
    free(system);
}

void spring_bone_reset(SpringBoneSystem* system) {
    if (system)
        system->needs_reset = true;
}

int spring_bone_add_chain(SpringBoneSystem* system, const char* root_bone_name) {
    if (!system || !root_bone_name)
        return -1;

    Skeleton* skeleton = system->skeleton;
    int root_index = get_bone_index_by_name(skeleton, root_bone_name);
    if (root_index < 0) {
        return -1;
    }

    // Mark the descendant subtree (parent-first order makes this one pass)
    // and find each bone's first in-chain child, which provides its tip
    bool in_chain[MAX_BONES] = {false};
    int first_child[MAX_BONES];
    in_chain[root_index] = true;
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        first_child[i] = -1;
    }
    size_t new_joints = (system->bone_to_joint[root_index] < 0) ? 1 : 0;
    for (size_t i = (size_t)root_index + 1; i < skeleton->bone_count; i++) {
        int parent = skeleton->bones[i].parent_index;
        if (parent >= 0 && in_chain[parent]) {
            in_chain[i] = true;
            if (first_child[parent] < 0) {
                first_child[parent] = (int)i;
            }
            if (system->bone_to_joint[i] < 0) {
                new_joints++;
            }
        }
    }
    if (new_joints == 0)
        return 0;

    // All-or-nothing: allocate everything before committing any state
    SpringBoneJoint* joints =
        realloc(system->joints, (system->joint_count + new_joints) * sizeof(SpringBoneJoint));
    if (!joints) {
        log_error("Failed to allocate spring bone joints");
        return -1;
    }
    system->joints = joints;

    mat4* bind_globals = malloc(skeleton->bone_count * sizeof(mat4));
    if (!bind_globals) {
        log_error("Failed to allocate bind globals for spring chain");
        return -1;
    }
    skeleton_compute_bind_globals(skeleton, bind_globals);

    int added = 0;
    for (size_t i = (size_t)root_index; i < skeleton->bone_count; i++) {
        if (!in_chain[i] || system->bone_to_joint[i] >= 0)
            continue;

        SpringBoneJoint* joint = &system->joints[system->joint_count];
        joint->bone_index = (int)i;
        glm_vec3_zero(joint->curr_tip);
        glm_vec3_zero(joint->prev_tip);

        if (first_child[i] >= 0) {
            // Tip = the in-chain child's origin, already in this bone's frame
            glm_vec3_copy(skeleton->bones[first_child[i]].local_transform[3],
                          joint->tip_offset_local);
        } else {
            // Leaf: continue the chain direction past this bone's head
            vec3 head, parent_head, tip_model;
            glm_vec3_copy(bind_globals[i][3], head);
            int parent = skeleton->bones[i].parent_index;
            if (parent >= 0) {
                glm_vec3_copy(bind_globals[parent][3], parent_head);
            } else {
                glm_vec3_copy(head, parent_head);
            }
            vec3 dir;
            glm_vec3_sub(head, parent_head, dir);
            if (glm_vec3_norm(dir) < 1e-6f) {
                glm_vec3_copy((vec3){0.0f, 0.05f, 0.0f}, dir);
            }
            glm_vec3_add(head, dir, tip_model);

            mat4 inv_bind;
            glm_mat4_inv(bind_globals[i], inv_bind);
            glm_mat4_mulv3(inv_bind, tip_model, 1.0f, joint->tip_offset_local);
        }

        system->bone_to_joint[i] = (int)system->joint_count;
        system->joint_count++;
        added++;
    }

    free(bind_globals);

    // New joints must snap to their targets before simulating
    system->needs_reset = true;

    return added;
}

int spring_bone_add_chains_by_prefix(SpringBoneSystem* system, const char* prefix) {
    if (!system || !prefix)
        return 0;

    Skeleton* skeleton = system->skeleton;
    size_t prefix_len = strlen(prefix);
    int chains_added = 0;

    for (size_t i = 0; i < skeleton->bone_count; i++) {
        const Bone* bone = &skeleton->bones[i];
        if (!bone->name || strncmp(bone->name, prefix, prefix_len) != 0)
            continue;

        // A chain root's parent must not match the prefix
        int parent = bone->parent_index;
        if (parent >= 0 && skeleton->bones[parent].name &&
            strncmp(skeleton->bones[parent].name, prefix, prefix_len) == 0)
            continue;

        if (spring_bone_add_chain(system, bone->name) > 0) {
            chains_added++;
        }
    }

    return chains_added;
}

/*
 * Constrain the simulated tip onto the sphere around the bone head
 */
static void constrain_tip(SpringBoneJoint* joint, vec3 head, vec3 tip_target, float rest_len,
                          float max_stretch) {
    vec3 dir;
    glm_vec3_sub(joint->curr_tip, head, dir);
    float len = glm_vec3_norm(dir);
    if (len < 1e-8f) {
        glm_vec3_copy(tip_target, joint->curr_tip);
        return;
    }
    float clamped = glm_clamp(len, rest_len, rest_len * (1.0f + max_stretch));
    glm_vec3_scale(dir, clamped / len, dir);
    glm_vec3_add(head, dir, joint->curr_tip);
}

/*
 * Limit the swing angle between the animated direction and the simulated
 * direction. Applied every substep so the deviation can never approach 180
 * degrees, where the swing rotation axis becomes unstable (visible as the
 * bone spinning erratically), and so stiff items keep their shape.
 */
static void clamp_swing_angle(SpringBoneJoint* joint, vec3 head, vec3 tip_target,
                              float max_angle_deg) {
    vec3 dir_target, dir_sim;
    glm_vec3_sub(tip_target, head, dir_target);
    glm_vec3_normalize(dir_target);
    glm_vec3_sub(joint->curr_tip, head, dir_sim);
    float sim_len = glm_vec3_norm(dir_sim);
    if (sim_len < 1e-8f)
        return;
    glm_vec3_scale(dir_sim, 1.0f / sim_len, dir_sim);

    float max_angle = glm_rad(max_angle_deg);
    float angle = glm_vec3_angle(dir_target, dir_sim);
    if (angle <= max_angle)
        return;

    // Rotate the target direction only up to the limit toward the sim
    versor full_swing, limited_swing, identity;
    glm_quat_from_vecs(dir_target, dir_sim, full_swing);
    glm_quat_identity(identity);
    glm_quat_slerp(identity, full_swing, max_angle / angle, limited_swing);

    vec3 limited_dir;
    glm_quat_rotatev(limited_swing, dir_target, limited_dir);
    glm_vec3_scale(limited_dir, sim_len, limited_dir);
    glm_vec3_add(head, limited_dir, joint->curr_tip);
}

void spring_bone_update(SpringBoneSystem* system, mat4* local_transforms, mat4* global_transforms,
                        float delta_time) {
    if (!system || !system->enabled || system->joint_count == 0 || !local_transforms ||
        !global_transforms)
        return;

    Skeleton* skeleton = system->skeleton;
    const SpringBoneParams* params = &system->params;

    if (delta_time < 0.0f)
        delta_time = 0.0f;
    if (delta_time > SPRING_MAX_DT)
        delta_time = SPRING_MAX_DT;

    // time_accum stays below SPRING_STEP after the subtraction, so with the
    // dt clamp above, steps can never exceed SPRING_MAX_SUBSTEPS
    system->time_accum += delta_time;
    int steps = (int)(system->time_accum / SPRING_STEP);
    system->time_accum -= (float)steps * SPRING_STEP;

    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        int j = system->bone_to_joint[i];

        if (j < 0) {
            // Re-accumulate from the parent: identical result for bones with
            // no simulated ancestor, corrected result for descendants of one
            if (bone->parent_index >= 0) {
                glm_mat4_mul(global_transforms[bone->parent_index], local_transforms[i],
                             global_transforms[i]);
            }
            continue;
        }

        SpringBoneJoint* joint = &system->joints[j];

        // Animated target from the (possibly spring-modified) parent global
        mat4 target;
        if (bone->parent_index >= 0) {
            glm_mat4_mul(global_transforms[bone->parent_index], local_transforms[i], target);
        } else {
            glm_mat4_copy(local_transforms[i], target);
        }

        vec3 head, tip_target;
        glm_vec3_copy(target[3], head);
        glm_mat4_mulv3(target, joint->tip_offset_local, 1.0f, tip_target);
        float rest_len = glm_vec3_distance(head, tip_target);
        if (rest_len < 1e-6f) {
            // Degenerate bone: follow the animation rigidly
            glm_mat4_copy(target, global_transforms[i]);
            continue;
        }

        // Reset and teleport snap
        if (system->needs_reset ||
            glm_vec3_distance(tip_target, joint->curr_tip) > params->teleport_distance) {
            glm_vec3_copy(tip_target, joint->curr_tip);
            glm_vec3_copy(tip_target, joint->prev_tip);
        }

        // Verlet substeps
        for (int s = 0; s < steps; s++) {
            vec3 velocity, pull;
            glm_vec3_sub(joint->curr_tip, joint->prev_tip, velocity);
            glm_vec3_scale(velocity, 1.0f - params->damping, velocity);
            glm_vec3_sub(tip_target, joint->curr_tip, pull);
            glm_vec3_scale(pull, params->stiffness * 0.5f, pull);

            glm_vec3_copy(joint->curr_tip, joint->prev_tip);
            glm_vec3_add(joint->curr_tip, velocity, joint->curr_tip);
            glm_vec3_add(joint->curr_tip, pull, joint->curr_tip);
            joint->curr_tip[1] -= params->gravity * SPRING_STEP * SPRING_STEP;

            constrain_tip(joint, head, tip_target, rest_len, params->max_stretch);
            clamp_swing_angle(joint, head, tip_target, params->max_angle_deg);
        }

        // NOTE: no constraint on zero-substep frames. The swing below only
        // uses the tip DIRECTION, so nothing stretches visually, and moving
        // the tip against a fresh head without advancing prev_tip would
        // inject phantom velocity into the next substep at high framerates.

        // Swing the animated orientation so the tip lands on the simulation
        vec3 dir_target, dir_sim;
        glm_vec3_sub(tip_target, head, dir_target);
        glm_vec3_normalize(dir_target);
        glm_vec3_sub(joint->curr_tip, head, dir_sim);
        if (glm_vec3_norm(dir_sim) < 1e-8f) {
            // Head landed on the stale tip: follow rigidly this frame
            glm_mat4_copy(target, global_transforms[i]);
            continue;
        }
        glm_vec3_normalize(dir_sim);

        versor swing;
        glm_quat_from_vecs(dir_target, dir_sim, swing);
        mat4 rotation;
        glm_quat_mat4(swing, rotation);
        glm_mat4_mul(rotation, target, global_transforms[i]);
        glm_vec3_copy(head, global_transforms[i][3]);
        global_transforms[i][3][3] = 1.0f;
    }

    system->needs_reset = false;
}
