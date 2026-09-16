#ifndef RIGGING_H
#define RIGGING_H

#include <stdbool.h>
#include "animation.h"

// Bone categories for semantic matching
typedef enum {
    BCAT_UNKNOWN = 0,
    BCAT_ROOT,
    BCAT_HIPS,
    BCAT_SPINE,
    BCAT_NECK,
    BCAT_HEAD,
    BCAT_SHOULDER,  // clavicle
    BCAT_UPPER_ARM, // humerus
    BCAT_LOWER_ARM, // radius/ulna
    BCAT_HAND,
    BCAT_FINGER,
    BCAT_UPPER_LEG, // femur/thigh
    BCAT_LOWER_LEG, // tibia/shin
    BCAT_FOOT,
    BCAT_TOE,
} BoneCategory;

// Normalize bone name: strip prefixes/suffixes, lowercase
// Returns NULL if bone should be skipped (e.g., Hips translation channel)
// Caller must free the returned string
char* normalize_bone_name(const char* name);

// Check if normalized name contains substring
bool bone_contains(const char* name, const char* sub);

// Categorize bone by semantic meaning
BoneCategory categorize_bone(const char* normalized_name);

// Get side: 0=center, 1=left, 2=right
int get_bone_side(const char* normalized_name);

// Get position in chain: 0=unknown, 1=lower/first, 2=middle/second, 3=upper/third
int get_bone_position(const char* normalized_name, BoneCategory category);

// Get finger identity: 0=none, 1=thumb, 2=index, 3=middle, 4=ring, 5=pinky
int get_finger_id(const char* normalized_name);

// Find skeleton bone that semantically matches animation bone name
// Returns bone index if EXACTLY ONE match found, -1 if no match or ambiguous
// Note: Requires compatible skeleton orientations for animations to work correctly
int find_matching_bone_smart(Skeleton* skeleton, const char* anim_bone_name);

// The bone a caller MEANS by `name`: the exact name where the skeleton carries it, the
// semantic match otherwise. -1 when neither answers.
//
// Exact FIRST, and that order is the whole safety of it: every rig that already resolved
// keeps the index it had, so nothing a caller does today can move. The fallback only runs
// where the exact lookup has already failed -- which is where a caller was about to give
// up anyway, and where the difference is a feature working at all rather than working
// differently.
//
// This is what lets a subsystem name bones in ONE vocabulary and still reach a rig that
// spells them another way. The import path has matched semantically since cross-rig
// retargeting existed; everything downstream of it -- the IK chains, the layer masks,
// the spring roots -- asked for literal strings and silently got nothing on a rig whose
// author named a foot `leg left ankle`.
int skeleton_resolve_bone(Skeleton* skeleton, const char* name);

// The bone a rig's ROOT MOTION belongs to: the hips where the rig has them, resolved
// the way everything else here resolves, and the first bone with no parent otherwise.
// -1 for an empty skeleton.
//
// The fallback is what makes every rig answer, and it is also why a clip translating
// that bone cannot be taken as root motion on its own -- a one-bone rig sliding
// sideways is a legitimate animation and not a character walking. Whether the travel
// belongs to the character is the caller's to state (`Animator.root_motion`).
int skeleton_root_bone(Skeleton* skeleton);

#endif // RIGGING_H
