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

#endif // RIGGING_H
