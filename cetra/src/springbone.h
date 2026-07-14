#ifndef _SPRINGBONE_H_
#define _SPRINGBONE_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stddef.h>

#include "animation.h"

/*
 * Spring bones: procedural secondary motion for bone chains that have no
 * animation channels (hair, straps, dangling props). Each registered bone's
 * tip is simulated as a damped Verlet particle pulled toward its animated
 * rest position and constrained to the bone length; the resulting swing
 * rotation is applied on top of the animated pose between animation sampling
 * and skinning matrix computation.
 */

typedef struct SpringBoneParams {
    float stiffness;         // 0..1 pull fraction toward the animated pose per substep
    float damping;           // 0..1 velocity kill factor (0 = floppy, 1 = no inertia)
    float gravity;           // m/s^2 along model-space -Y
    float max_stretch;       // allowed stretch fraction beyond rest length
    float max_angle_deg;     // max swing away from the animated pose (degrees)
    float teleport_distance; // snap the sim when the target jumps farther (meters)
} SpringBoneParams;

typedef struct SpringBoneJoint {
    int bone_index;        // index into skeleton->bones
    vec3 tip_offset_local; // tip position in this bone's local frame
    vec3 curr_tip;         // simulated tip, model space (Verlet state)
    vec3 prev_tip;
} SpringBoneJoint;

typedef struct SpringBoneSystem {
    Skeleton* skeleton; // not owned
    SpringBoneJoint* joints;
    size_t joint_count;
    SpringBoneParams params;      // shared by all registered chains
    int bone_to_joint[MAX_BONES]; // -1 if bone not simulated
    bool enabled;
    bool needs_reset; // snap all joints to their targets on next update
    float time_accum; // fixed-substep accumulator
} SpringBoneSystem;

// Created with default params; adjust system->params directly to tune
SpringBoneSystem* create_spring_bone_system(Skeleton* skeleton);
void free_spring_bone_system(SpringBoneSystem* system);

SpringBoneParams spring_bone_default_params(void);

// Register the chain rooted at a bone (the whole descendant subtree).
// Returns the number of joints added, -1 on error.
int spring_bone_add_chain(SpringBoneSystem* system, const char* root_bone_name);

// Register a chain for every bone whose name starts with prefix and whose
// parent's name does not. Returns the number of chains added.
int spring_bone_add_chains_by_prefix(SpringBoneSystem* system, const char* prefix);

// Snap all joints back to their animated targets on the next update
void spring_bone_reset(SpringBoneSystem* system);

// Simulate and apply swing rotations. Called between animation sampling and
// skinning matrix computation with the state's scratch transform arrays.
void spring_bone_update(SpringBoneSystem* system, mat4* local_transforms, mat4* global_transforms,
                        float delta_time);

#endif // _SPRINGBONE_H_
