#include "rigging.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ext/log.h"

char* normalize_bone_name(const char* name) {
    if (!name)
        return NULL;

    const char* start = name;

    // Strip "mixamorig:" prefix
    if (strncmp(start, "mixamorig:", 10) == 0) {
        start += 10;
    }

    // Skip root translation channels (causes character to fly off screen)
    // Hips_$AssimpFbx$_Translation should not be mapped
    if (strstr(start, "Hips") && strstr(name, "_Translation")) {
        return NULL;
    }

    // Find end (stop at $AssimpFbx$ suffix)
    const char* end = strstr(start, "_$AssimpFbx$");
    if (!end)
        end = start + strlen(start);

    size_t len = end - start;
    char* result = malloc(len + 1);
    if (!result)
        return NULL;

    // Copy and lowercase, convert underscores to spaces
    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (c == '_')
            c = ' ';
        result[i] = tolower((unsigned char)c);
    }
    result[len] = '\0';

    return result;
}

bool bone_contains(const char* name, const char* sub) {
    return strstr(name, sub) != NULL;
}

BoneCategory categorize_bone(const char* norm) {
    // Skip control/helper bones
    if (bone_contains(norm, "ctrl") || bone_contains(norm, "ik") || bone_contains(norm, "pole") ||
        bone_contains(norm, "target") || bone_contains(norm, "helper")) {
        return BCAT_UNKNOWN;
    }

    // Order matters: check specific patterns before general ones

    // Fingers (must check before hand)
    if (bone_contains(norm, "finger") || bone_contains(norm, "thumb") ||
        bone_contains(norm, "index") || bone_contains(norm, "pinky") ||
        bone_contains(norm, "ring") ||
        (bone_contains(norm, "hand") &&
         (bone_contains(norm, "middle") || bone_contains(norm, "little")))) {
        return BCAT_FINGER;
    }

    // Toes
    if (bone_contains(norm, "toe"))
        return BCAT_TOE;

    // Foot/ankle
    if (bone_contains(norm, "foot") || bone_contains(norm, "ankle"))
        return BCAT_FOOT;

    // Upper leg (thigh)
    if (bone_contains(norm, "thigh") || bone_contains(norm, "upleg") ||
        bone_contains(norm, "upperleg")) {
        return BCAT_UPPER_LEG;
    }

    // Lower leg (shin/knee) - "leg" without "upleg"
    if ((bone_contains(norm, "leg") && !bone_contains(norm, "upleg") &&
         !bone_contains(norm, "upperleg")) ||
        bone_contains(norm, "knee") || bone_contains(norm, "shin") || bone_contains(norm, "calf")) {
        return BCAT_LOWER_LEG;
    }

    // Hand/wrist (but not if it's a finger bone)
    if (bone_contains(norm, "hand") || bone_contains(norm, "wrist"))
        return BCAT_HAND;

    // Lower arm (forearm/elbow)
    if (bone_contains(norm, "forearm") || bone_contains(norm, "elbow") ||
        bone_contains(norm, "lowerarm")) {
        return BCAT_LOWER_ARM;
    }

    // Shoulder with "2" = upper arm in some rigs
    if (bone_contains(norm, "shoulder")) {
        if (bone_contains(norm, "2") || bone_contains(norm, " 2")) {
            return BCAT_UPPER_ARM;
        }
        return BCAT_SHOULDER;
    }

    // Upper arm - "arm" without "forearm"
    if ((bone_contains(norm, "arm") && !bone_contains(norm, "forearm")) ||
        bone_contains(norm, "upperarm")) {
        return BCAT_UPPER_ARM;
    }

    // Neck - check before head since "head neck lower" is a neck bone
    if (bone_contains(norm, "neck"))
        return BCAT_NECK;

    // Head - but NOT facial features (jaw, eye, nose, lip, tongue, brow, etc.)
    if (bone_contains(norm, "head")) {
        if (bone_contains(norm, "jaw") || bone_contains(norm, "eye") ||
            bone_contains(norm, "nose") || bone_contains(norm, "lip") ||
            bone_contains(norm, "tongue") || bone_contains(norm, "brow") ||
            bone_contains(norm, "mouth") || bone_contains(norm, "lid") ||
            bone_contains(norm, "ball")) {
            return BCAT_UNKNOWN; // Facial feature, not main head bone
        }
        return BCAT_HEAD;
    }

    // Spine/chest
    if (bone_contains(norm, "spine") || bone_contains(norm, "chest"))
        return BCAT_SPINE;

    // Hips/pelvis
    if (bone_contains(norm, "hip") || bone_contains(norm, "pelvis"))
        return BCAT_HIPS;

    // Root
    if (bone_contains(norm, "root") && !bone_contains(norm, "hip"))
        return BCAT_ROOT;

    return BCAT_UNKNOWN;
}

int get_bone_side(const char* norm) {
    if (bone_contains(norm, "left") || bone_contains(norm, " l ") ||
        (norm[0] == 'l' && norm[1] == ' ')) {
        return 1;
    }
    if (bone_contains(norm, "right") || bone_contains(norm, " r ") ||
        (norm[0] == 'r' && norm[1] == ' ')) {
        return 2;
    }
    return 0;
}

int get_bone_position(const char* norm, BoneCategory cat) {
    // Word-based positions (but not for fingers - "middle" means finger 3, not position)
    if (bone_contains(norm, "lower") || bone_contains(norm, "base"))
        return 1;
    if (cat != BCAT_FINGER) {
        if (bone_contains(norm, "middle") || bone_contains(norm, "mid"))
            return 2;
    }
    if (bone_contains(norm, "upper") || bone_contains(norm, "top"))
        return 3;

    // Spine special case: Mixamo uses Spine, Spine1, Spine2
    // where Spine=lower(1), Spine1=middle(2), Spine2=upper(3)
    if (cat == BCAT_SPINE) {
        if (bone_contains(norm, "spine2") || bone_contains(norm, "spine 2"))
            return 3;
        if (bone_contains(norm, "spine1") || bone_contains(norm, "spine 1"))
            return 2;
        // Just "spine" without number = first spine
        if (bone_contains(norm, "spine") && !bone_contains(norm, "1") &&
            !bone_contains(norm, "2") && !bone_contains(norm, "3")) {
            return 1;
        }
    }

    // Finger segment: check trailing character
    if (cat == BCAT_FINGER) {
        size_t len = strlen(norm);
        if (len > 0) {
            char last = norm[len - 1];
            if (last == '1' || last == 'a')
                return 1;
            if (last == '2' || last == 'b')
                return 2;
            if (last == '3' || last == 'c')
                return 3;
        }
    }

    // Numeric position (isolated numbers)
    if (bone_contains(norm, " 1") || bone_contains(norm, "1 "))
        return 1;
    if (bone_contains(norm, " 2") || bone_contains(norm, "2 "))
        return 2;
    if (bone_contains(norm, " 3") || bone_contains(norm, "3 "))
        return 3;

    return 0;
}

int get_finger_id(const char* norm) {
    if (bone_contains(norm, "thumb"))
        return 1;
    if (bone_contains(norm, "index"))
        return 2;
    // "middle" for finger - check for handmiddle pattern (Mixamo style)
    if (bone_contains(norm, "handmiddle"))
        return 3;
    if (bone_contains(norm, "ring"))
        return 4;
    if (bone_contains(norm, "pinky") || bone_contains(norm, "little"))
        return 5;

    // Look for "finger N" pattern (custom rig style: "finger 3a")
    const char* fp = strstr(norm, "finger");
    if (fp) {
        fp += 6;
        while (*fp == ' ' || *fp == '_')
            fp++;
        if (*fp >= '1' && *fp <= '5') {
            return *fp - '0';
        }
    }

    return 0;
}

int find_matching_bone_smart(Skeleton* skeleton, const char* anim_bone_name) {
    if (!skeleton || !anim_bone_name || skeleton->bone_count == 0)
        return -1;

    // First try exact match
    int exact = get_bone_index_by_name(skeleton, anim_bone_name);
    if (exact >= 0)
        return exact;

    // Normalize and analyze animation bone
    char* anim_norm = normalize_bone_name(anim_bone_name);
    if (!anim_norm)
        return -1;

    BoneCategory anim_cat = categorize_bone(anim_norm);
    int anim_side = get_bone_side(anim_norm);
    int anim_pos = get_bone_position(anim_norm, anim_cat);
    int anim_finger = get_finger_id(anim_norm);

    // Unknown category = can't match
    if (anim_cat == BCAT_UNKNOWN) {
        free(anim_norm);
        return -1;
    }

    // Score all skeleton bones
    int best_index = -1;
    float best_score = 0.0f;
    int best_count = 0;
    char* best_name = NULL;

    for (size_t i = 0; i < skeleton->bone_count; i++) {
        char* skel_norm = normalize_bone_name(skeleton->bones[i].name);
        if (!skel_norm)
            continue;

        BoneCategory skel_cat = categorize_bone(skel_norm);
        int skel_side = get_bone_side(skel_norm);
        int skel_pos = get_bone_position(skel_norm, skel_cat);
        int skel_finger = get_finger_id(skel_norm);

        float score = 0.0f;

        // Category must match
        if (anim_cat != skel_cat) {
            free(skel_norm);
            continue;
        }
        score += 100.0f;

        // Side must match
        if (anim_side != skel_side) {
            free(skel_norm);
            continue;
        }
        if (anim_side != 0)
            score += 50.0f;

        // Finger ID must match for fingers
        if (anim_cat == BCAT_FINGER) {
            if (anim_finger != 0 && skel_finger != 0 && anim_finger != skel_finger) {
                free(skel_norm);
                continue;
            }
            if (anim_finger != 0 && anim_finger == skel_finger) {
                score += 40.0f;
            }
        }

        // Position matching
        if (anim_pos != 0 && skel_pos != 0) {
            if (anim_pos == skel_pos) {
                score += 30.0f;
            } else {
                score -= 50.0f; // Position mismatch = strong penalty
            }
        } else if (anim_pos == 0 && skel_pos != 0) {
            // Animation has no position, skeleton does - prefer lower/first
            // This handles Mixamo "Neck" -> "head neck lower" preference
            if (skel_pos == 1)
                score += 10.0f;
            else if (skel_pos == 2)
                score += 5.0f;
            // Position 3 (upper) gets no bonus
        }

        // Prefer "hips" over "pelvis" for hip bones
        if (anim_cat == BCAT_HIPS) {
            if (bone_contains(skel_norm, "hip"))
                score += 15.0f;
        }

        // Track best match
        if (score > best_score) {
            best_score = score;
            best_index = (int)i;
            best_count = 1;
            free(best_name);
            best_name = skel_norm;
            skel_norm = NULL;
        } else if (score == best_score && score > 0) {
            best_count++; // Tie
        }

        if (skel_norm)
            free(skel_norm);
    }

    free(anim_norm);

    // Only return if exactly one best match with high confidence
    if (best_count == 1 && best_score >= 100.0f) {
        log_info("Auto-mapped '%s' -> '%s' (score=%.0f)", anim_bone_name,
                 best_name ? best_name : "?", best_score);
        free(best_name);
        return best_index;
    }

    if (best_count > 1) {
        log_warn("Ambiguous bone match for '%s' (%d candidates)", anim_bone_name, best_count);
    }

    free(best_name);
    return -1;
}
