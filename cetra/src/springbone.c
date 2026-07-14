#include "springbone.h"
#include "ext/log.h"

#include <stdlib.h>
#include <string.h>

// Fixed simulation substep so behavior is framerate independent
#define SPRING_STEP (1.0f / 120.0f)
#define SPRING_MAX_SUBSTEPS 4
#define SPRING_MAX_DT 0.033f

SpringBoneParams spring_bone_default_params(void) {
    SpringBoneParams params = {
        .stiffness = 0.15f,
        .damping = 0.2f,
        .gravity = 9.8f,
        .max_stretch = 0.0f,
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
    system->chains = NULL;
    system->chain_count = 0;
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

    for (size_t i = 0; i < system->chain_count; i++) {
        free(system->chains[i].root_name);
    }
    free(system->chains);
    free(system->joints);
    free(system);
}

void spring_bone_reset(SpringBoneSystem* system) {
    if (system)
        system->needs_reset = true;
}

/*
 * Compute bind-pose global transforms for the whole skeleton (bones are
 * ordered parent-first, so a single accumulation pass suffices).
 */
static void compute_bind_globals(Skeleton* skeleton, mat4* globals) {
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        if (bone->parent_index < 0 || (size_t)bone->parent_index >= skeleton->bone_count) {
            glm_mat4_copy(bone->local_transform, globals[i]);
        } else {
            glm_mat4_mul(globals[bone->parent_index], bone->local_transform, globals[i]);
        }
    }
}

int spring_bone_add_chain(SpringBoneSystem* system, const char* root_bone_name,
                          const SpringBoneParams* params) {
    if (!system || !root_bone_name || !params)
        return -1;

    Skeleton* skeleton = system->skeleton;
    int root_index = get_bone_index_by_name(skeleton, root_bone_name);
    if (root_index < 0) {
        return -1;
    }

    // Mark the descendant subtree (parent-first order makes this one pass)
    bool in_chain[MAX_BONES] = {false};
    in_chain[root_index] = true;
    for (size_t i = (size_t)root_index + 1; i < skeleton->bone_count; i++) {
        int parent = skeleton->bones[i].parent_index;
        if (parent >= 0 && in_chain[parent]) {
            in_chain[i] = true;
        }
    }

    // First in-chain child of each bone provides the tip offset
    int first_child[MAX_BONES];
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        first_child[i] = -1;
    }
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        int parent = skeleton->bones[i].parent_index;
        if (in_chain[i] && parent >= 0 && in_chain[parent] && first_child[parent] < 0) {
            first_child[parent] = (int)i;
        }
    }

    mat4* bind_globals = malloc(skeleton->bone_count * sizeof(mat4));
    if (!bind_globals) {
        log_error("Failed to allocate bind globals for spring chain");
        return -1;
    }
    compute_bind_globals(skeleton, bind_globals);

    int added = 0;
    for (size_t i = (size_t)root_index; i < skeleton->bone_count; i++) {
        if (!in_chain[i] || system->bone_to_joint[i] >= 0)
            continue;

        SpringBoneJoint* new_joints =
            realloc(system->joints, (system->joint_count + 1) * sizeof(SpringBoneJoint));
        if (!new_joints) {
            log_error("Failed to allocate spring bone joint");
            free(bind_globals);
            return added > 0 ? added : -1;
        }
        system->joints = new_joints;

        SpringBoneJoint* joint = &system->joints[system->joint_count];
        joint->bone_index = (int)i;
        joint->chain_index = (int)system->chain_count;
        glm_vec3_zero(joint->curr_tip);
        glm_vec3_zero(joint->prev_tip);
        joint->initialized = false;

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

    if (added == 0)
        return 0;

    SpringBoneChain* new_chains =
        realloc(system->chains, (system->chain_count + 1) * sizeof(SpringBoneChain));
    if (!new_chains) {
        log_error("Failed to allocate spring bone chain");
        return added;
    }
    system->chains = new_chains;

    SpringBoneChain* chain = &system->chains[system->chain_count];
    chain->root_name = strdup(root_bone_name);
    chain->first_joint = (int)(system->joint_count - (size_t)added);
    chain->joint_count = added;
    chain->params = *params;
    system->chain_count++;

    return added;
}

int spring_bone_add_chains_by_prefix(SpringBoneSystem* system, const char* prefix,
                                     const SpringBoneParams* params) {
    if (!system || !prefix || !params)
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

        if (spring_bone_add_chain(system, bone->name, params) > 0) {
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

void spring_bone_update(SpringBoneSystem* system, mat4* local_transforms, mat4* global_transforms,
                        float delta_time) {
    if (!system || !system->enabled || system->joint_count == 0 || !local_transforms ||
        !global_transforms)
        return;

    Skeleton* skeleton = system->skeleton;

    if (delta_time < 0.0f)
        delta_time = 0.0f;
    if (delta_time > SPRING_MAX_DT)
        delta_time = SPRING_MAX_DT;

    system->time_accum += delta_time;
    int steps = (int)(system->time_accum / SPRING_STEP);
    if (steps > SPRING_MAX_SUBSTEPS)
        steps = SPRING_MAX_SUBSTEPS;
    system->time_accum -= (float)steps * SPRING_STEP;
    if (system->time_accum > SPRING_STEP)
        system->time_accum = SPRING_STEP;

    bool dirty[MAX_BONES] = {false};

    for (size_t i = 0; i < skeleton->bone_count; i++) {
        Bone* bone = &skeleton->bones[i];
        int j = system->bone_to_joint[i];

        if (j < 0) {
            // Not simulated: re-accumulate if an ancestor was modified
            if (bone->parent_index >= 0 && dirty[bone->parent_index]) {
                glm_mat4_mul(global_transforms[bone->parent_index], local_transforms[i],
                             global_transforms[i]);
                dirty[i] = true;
            }
            continue;
        }

        SpringBoneJoint* joint = &system->joints[j];
        const SpringBoneParams* params = &system->chains[joint->chain_index].params;

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
            dirty[i] = true;
            continue;
        }

        // Initialization and teleport snap
        if (!joint->initialized || system->needs_reset ||
            glm_vec3_distance(tip_target, joint->curr_tip) > params->teleport_distance) {
            glm_vec3_copy(tip_target, joint->curr_tip);
            glm_vec3_copy(tip_target, joint->prev_tip);
            joint->initialized = true;
        }

        // Verlet substeps
        for (int s = 0; s < steps; s++) {
            vec3 velocity, pull;
            glm_vec3_sub(joint->curr_tip, joint->prev_tip, velocity);
            glm_vec3_scale(velocity, 1.0f - params->damping, velocity);
            glm_vec3_sub(tip_target, joint->curr_tip, pull);
            glm_vec3_scale(pull, params->stiffness * (SPRING_STEP * 60.0f), pull);

            glm_vec3_copy(joint->curr_tip, joint->prev_tip);
            glm_vec3_add(joint->curr_tip, velocity, joint->curr_tip);
            glm_vec3_add(joint->curr_tip, pull, joint->curr_tip);
            joint->curr_tip[1] -= params->gravity * SPRING_STEP * SPRING_STEP;

            constrain_tip(joint, head, tip_target, rest_len, params->max_stretch);
        }

        // Keep the tip on the sphere even on frames with no substep (the
        // head moves with the animation every frame)
        constrain_tip(joint, head, tip_target, rest_len, params->max_stretch);

        // Swing the animated orientation so the tip lands on the simulation
        vec3 dir_target, dir_sim;
        glm_vec3_sub(tip_target, head, dir_target);
        glm_vec3_normalize(dir_target);
        glm_vec3_sub(joint->curr_tip, head, dir_sim);
        glm_vec3_normalize(dir_sim);

        versor swing;
        glm_quat_from_vecs(dir_target, dir_sim, swing);
        mat4 rotation;
        glm_quat_mat4(swing, rotation);
        glm_mat4_mul(rotation, target, global_transforms[i]);
        glm_vec3_copy(head, global_transforms[i][3]);
        global_transforms[i][3][3] = 1.0f;

        // Keep the local transform consistent for downstream readers
        if (bone->parent_index >= 0) {
            mat4 parent_inv;
            glm_mat4_inv(global_transforms[bone->parent_index], parent_inv);
            glm_mat4_mul(parent_inv, global_transforms[i], local_transforms[i]);
        } else {
            glm_mat4_copy(global_transforms[i], local_transforms[i]);
        }

        dirty[i] = true;
    }

    system->needs_reset = false;
}

int animation_add_spring_chain(AnimationState* state, const char* root_bone_name,
                               const SpringBoneParams* params) {
    if (!state || !state->skeleton)
        return -1;
    if (!state->springs) {
        state->springs = create_spring_bone_system(state->skeleton);
        if (!state->springs)
            return -1;
    }
    return spring_bone_add_chain(state->springs, root_bone_name, params);
}

int animation_add_spring_chains_by_prefix(AnimationState* state, const char* prefix,
                                          const SpringBoneParams* params) {
    if (!state || !state->skeleton)
        return 0;
    if (!state->springs) {
        state->springs = create_spring_bone_system(state->skeleton);
        if (!state->springs)
            return 0;
    }
    return spring_bone_add_chains_by_prefix(state->springs, prefix, params);
}
