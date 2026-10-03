#ifndef _LOOK_AT_H_
#define _LOOK_AT_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "animation.h"

/*
 * Turning a head toward a point (spec 13.17). A chain of up to LOOK_AT_MAX_BONES bones, root
 * first -- a neck and then a head -- each taking a share of the one rotation that brings the
 * last bone's aim onto the target, and each carrying everything below it rigidly.
 *
 * It runs at ik.h's seam, after the solve and before the skinning matrices are built, and
 * writes global_transforms and nothing else, for ik.h's reason: a rotation written into the
 * Pose is thrown away for every bone no clip drives. After the solve rather than before,
 * because the IK may drop the pelvis the whole neck hangs from.
 *
 * The rotation is a YAW about the model's up and then a PITCH about the level axis across
 * the new aim, never the shortest arc between the two aims. The shortest arc from an aim
 * that differs in both heading and elevation ROLLS the head on the way, and a gaze that
 * stays level is what any animal holding its head up does; a cocked head is something to
 * author, not a by-product of the arithmetic. It also leaves the clip's own roll alone.
 *
 * The limits are measured from the chain PARENT's forward, which is what "how far can the
 * neck turn" means: a body walking past something turns its head further and further round
 * until the target passes give_up, and then the head goes back to the clip.
 *
 * Gaze is eased as a WORLD direction rather than as angles in the parent's frame, so a body
 * that turns under a steady gaze leaves the head on its target the way an animal's does,
 * instead of carrying it round and catching up.
 *
 * Nothing runs while the weight is zero, so an installed system with no target leaves every
 * pose exactly as the clip and the IK made it.
 */

#define LOOK_AT_MAX_BONES 4

typedef struct LookAtBone {
    int bone;
    float share;                // of the rotation; the shares need not sum to 1
    uint8_t subtree[MAX_BONES]; // this bone and every bone below it
} LookAtBone;

typedef struct LookAtSystem {
    // ENGINE-OWNED
    Skeleton* skeleton;
    LookAtBone bones[LOOK_AT_MAX_BONES];
    int bone_count;
    int parent; // the first bone's parent, whose forward the limits are measured from; -1 is
                // the model's own forward
    mat4 model_to_world, world_to_model;
    vec3 target; // world, or model space until a world is set
    bool has_target;
    bool tracking; // following the target, which it stops past give_up
    float weight;  // how far the head is turned from the clip toward the target, 0..1
    vec3 gaze;     // the eased world direction the head is turned toward
    bool gaze_valid;

    // BY FUNCTION: look_at_add_bone, look_at_set_world, look_at_set_target,
    // look_at_clear_target.

    // SETTINGS
    vec3 forward;     // the way the rig faces in model space; all zero is +z
    vec3 up;          // the rig's up in model space; all zero is +y
    vec3 eye;         // the bind-pose point a gaze is measured from; all zero is the last bone's
                      // head
    float max_yaw;    // radians either side of the parent's forward
    float max_pitch;  // radians above and below it
    float give_up;    // radians: a target further round than this is let go
    float rate;       // how fast the gaze follows a moving target, per second
    float blend_rate; // how fast the head turns to a target and back from one, per second
} LookAtSystem;

// A system on this skeleton with no bones and no target, at limits of 70 degrees either way
// and 40 up and down, letting go past 110.
LookAtSystem* create_look_at_system(Skeleton* skeleton);
void free_look_at_system(LookAtSystem* system);

// Append a bone to the chain, root first: each must descend from the one before. False, with
// a log line, when the name is not on the skeleton, the chain is full, or it does not descend.
bool look_at_add_bone(LookAtSystem* system, const char* name, float share);

// Where the skeleton's model space is in the world this frame. Until it is called the target
// is in model space, and a target held in model space rides along with the body.
void look_at_set_world(LookAtSystem* system, mat4 model_to_world);

// Look at a world point, from now until cleared or replaced.
void look_at_set_target(LookAtSystem* system, const vec3 world);
// Let the head go back to the clip.
void look_at_clear_target(LookAtSystem* system);

// Turn the chain toward the target on the pose's globals: the seam inside
// animation_state_apply_pose. dt drives the easing; 0 holds it where it is.
void look_at_solve(LookAtSystem* system, mat4* global_transforms, float dt);

#endif // _LOOK_AT_H_
