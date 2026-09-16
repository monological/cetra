#include "ragdoll.h"

#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "game/physics.h"
#include "mesh.h"
#include "ragdoll_jolt.h"
#include "rigging.h"

// The layer ragdoll bodies take, restated in ragdoll_jolt.cpp because that file
// is the engine's and this enum is the game layer's. Checked here, where both
// are visible, so a reordering of PhysicsLayer is a compile error and not a
// ragdoll that quietly stops colliding with the world.
_Static_assert((int)OBJ_LAYER_DYNAMIC == 1, "ragdoll_jolt.cpp's kRagdollLayer must match");

// A radius for a bone the mesh could not measure, as a fraction of its own
// length. Thin enough to read as a limb and never zero, which CapsuleShape
// asserts on.
#define RAGDOLL_FALLBACK_RATIO 0.18f
// Below this a bone is not a limb. A zero-length bone is a rig's control or
// twist joint; building a capsule for one is what the Jolt layer refuses.
#define RAGDOLL_MIN_LENGTH 1e-4f

typedef struct RagdollPart {
    int bone;   // index into the skeleton, -1 if unresolved
    int parent; // index into this array, -1 for the root
    float radius;
    float half_height;
    // The bind transform that takes the body's own frame to the bone's, so a
    // capsule centred on its limb can be turned back into a bone global.
    mat4 body_from_bone;
} RagdollPart;

struct RagdollSystem {
    Skeleton* skeleton; // borrowed
    RagdollPart parts[RAGDOLL_BONE_COUNT];
    int count;
    float node_scale;

    JoltRagdoll* jolt;
    mat4 to_world;
    mat4 to_model;
    bool active;
};

/*
 * Some bones have to be WALKED rather than named, and this is the one piece of
 * real design in the build.
 *
 * categorize_bone gives one category to a whole chain -- BCAT_SPINE covers
 * "spine" and "chest" alike, BCAT_HEAD covers "Head" and "HeadTop_End" -- and
 * the semantic matcher returns a bone only when EXACTLY ONE matches, so on any
 * rig with a segmented spine or a head tip it correctly refuses to pick. That
 * is not a defect in the matcher: asking it for "the chest" is asking a
 * question the name alone does not answer. get_bone_position does disambiguate
 * a spine, but by MIXAMO NAME PATTERN, so a rig spelling the chain
 * Spine/Chest/UpperChest gets a different answer than one spelling it
 * Spine/Spine1/Spine2.
 *
 * Walking asks what the hierarchy always knows: which bones of this category
 * descend from that ancestor, and which of them is nearest to it or furthest
 * from it. Parent-first ordering means the first found is nearest and the last
 * is furthest, with no depth arithmetic.
 */
static int find_category_under(const Skeleton* skeleton, int ancestor, BoneCategory want,
                               bool furthest) {
    int found = -1;
    if (ancestor < 0) {
        return -1;
    }
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        if ((int)i == ancestor) {
            continue;
        }
        char* norm = normalize_bone_name(skeleton->bones[i].name);
        if (!norm) {
            continue;
        }
        const BoneCategory cat = categorize_bone(norm);
        free(norm);
        if (cat != want) {
            continue;
        }
        // Descended from the ancestor, so an unrelated bone of the same
        // category on a prop or a second skeleton cannot be picked up.
        int walk = skeleton->bones[i].parent_index;
        bool under = false;
        while (walk >= 0) {
            if (walk == ancestor) {
                under = true;
                break;
            }
            walk = skeleton->bones[walk].parent_index;
        }
        if (!under) {
            continue;
        }
        if (found < 0 || furthest) {
            found = (int)i;
        }
        if (!furthest) {
            break;
        }
    }
    return found;
}

// The bone a category and side name, through the resolver that reaches a rig
// spelling it another way.
static int resolve_side(Skeleton* skeleton, const char* name) {
    return skeleton_resolve_bone(skeleton, name);
}

// The child a limb points at, so a capsule can span the bone rather than sit on
// its joint. The first child in the skeleton, which on a limb chain is the only
// one; -1 for a tip like a head, whose own length has to be guessed.
static int first_child(const Skeleton* skeleton, int bone) {
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        if (skeleton->bones[i].parent_index == bone) {
            return (int)i;
        }
    }
    return -1;
}

// The radius the mesh implies for a bone: half the SMALLER of its bind box's
// two cross-section extents, so a flat box gives a thin limb rather than a wide
// one. 0 when the mesh cannot answer.
static float measured_radius(const struct Mesh* mesh, int bone, float length) {
    if (!mesh || !mesh->bone_aabb || bone < 0 || (size_t)bone >= mesh->bone_aabb_count) {
        return 0.0f;
    }
    const AABB box = mesh->bone_aabb[bone];
    if (aabb_is_empty(&box)) {
        return 0.0f;
    }
    (void)length;
    const float ex = (box.max[0] - box.min[0]) * 0.5f;
    const float ey = (box.max[1] - box.min[1]) * 0.5f;
    const float ez = (box.max[2] - box.min[2]) * 0.5f;
    // The limb runs along its box's LONGEST extent, so its cross-section is the
    // other two and the radius is their mean. Chosen by value rather than by
    // axis, because a bone's local axes are the rig author's and not a
    // convention this can rely on -- dropping the largest is the whole rule.
    float longest = ex;
    if (ey > longest) {
        longest = ey;
    }
    if (ez > longest) {
        longest = ez;
    }
    return (ex + ey + ez - longest) * 0.5f;
}

RagdollSystem* create_ragdoll(Skeleton* skeleton, const struct Mesh* mesh, float node_scale) {
    if (!skeleton || skeleton->bone_count == 0) {
        log_error("ragdoll: no skeleton");
        return NULL;
    }
    if (!(node_scale > 0.0f)) {
        log_error("ragdoll: node scale %.6f is not positive", (double)node_scale);
        return NULL;
    }

    const int hips = skeleton_resolve_bone(skeleton, "hips");
    if (hips < 0) {
        log_error("ragdoll: '%s' has no hips; a humanoid ragdoll needs a root to hang from",
                  skeleton->name ? skeleton->name : "?");
        return NULL;
    }

    // Nearest and furthest of the spine chain, then the head nearest the top of
    // it -- "Head" rather than the "HeadTop_End" tip a Mixamo rig carries.
    const int spine = find_category_under(skeleton, hips, BCAT_SPINE, false);
    const int chest = find_category_under(skeleton, hips, BCAT_SPINE, true);
    const int head = find_category_under(skeleton, (chest >= 0) ? chest : hips, BCAT_HEAD, false);

    RagdollSystem* rd = calloc(1, sizeof(RagdollSystem));
    if (!rd) {
        log_error("ragdoll: out of memory");
        return NULL;
    }
    rd->skeleton = skeleton;
    rd->node_scale = node_scale;
    glm_mat4_identity(rd->to_world);
    glm_mat4_identity(rd->to_model);

    // The bone each body comes from, and which body it hangs off. A -1 bone
    // collapses the row out of the build below, which is how a one-segment
    // spine yields eleven bodies rather than a hole.
    struct {
        RagdollBone slot;
        int bone;
        RagdollBone parent_slot;
    } wanted[RAGDOLL_BONE_COUNT] = {
        {RAGDOLL_HIPS, hips, RAGDOLL_HIPS},
        {RAGDOLL_SPINE, spine, RAGDOLL_HIPS},
        {RAGDOLL_CHEST, (chest != spine) ? chest : -1, RAGDOLL_SPINE},
        {RAGDOLL_HEAD, head, RAGDOLL_CHEST},
        {RAGDOLL_UPPER_ARM_L, resolve_side(skeleton, "leftarm"), RAGDOLL_CHEST},
        {RAGDOLL_LOWER_ARM_L, resolve_side(skeleton, "leftforearm"), RAGDOLL_UPPER_ARM_L},
        {RAGDOLL_UPPER_ARM_R, resolve_side(skeleton, "rightarm"), RAGDOLL_CHEST},
        {RAGDOLL_LOWER_ARM_R, resolve_side(skeleton, "rightforearm"), RAGDOLL_UPPER_ARM_R},
        {RAGDOLL_THIGH_L, resolve_side(skeleton, "leftupleg"), RAGDOLL_HIPS},
        {RAGDOLL_SHIN_L, resolve_side(skeleton, "leftleg"), RAGDOLL_THIGH_L},
        {RAGDOLL_THIGH_R, resolve_side(skeleton, "rightupleg"), RAGDOLL_HIPS},
        {RAGDOLL_SHIN_R, resolve_side(skeleton, "rightleg"), RAGDOLL_THIGH_R},
    };

    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        rd->parts[i].bone = -1;
        rd->parts[i].parent = -1;
    }

    mat4* bind = calloc(skeleton->bone_count, sizeof(mat4));
    if (!bind) {
        log_error("ragdoll: out of memory");
        free(rd);
        return NULL;
    }
    skeleton_compute_bind_globals(skeleton, bind);

    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const int bone = wanted[i].bone;
        if (bone < 0) {
            continue;
        }

        // The limb's own length, from its bind head to its child's. A tip bone
        // has no child and takes its parent's separation instead, which is the
        // only measurement of it the rig carries.
        const int child = first_child(skeleton, bone);
        vec3 joint, tail;
        glm_vec3_copy(bind[bone][3], joint);
        if (child >= 0) {
            glm_vec3_copy(bind[child][3], tail);
        } else {
            const int parent = skeleton->bones[bone].parent_index;
            if (parent >= 0) {
                vec3 back;
                glm_vec3_sub(joint, bind[parent][3], back);
                glm_vec3_add(joint, back, tail);
            } else {
                glm_vec3_copy(joint, tail);
            }
        }

        vec3 along;
        glm_vec3_sub(tail, joint, along);
        const float length = glm_vec3_norm(along);
        if (length < RAGDOLL_MIN_LENGTH) {
            // A control or twist joint wearing a limb's name. Dropped rather
            // than built: the Jolt layer would refuse the capsule anyway, and
            // refusing the whole ragdoll over one auxiliary bone is worse than
            // simulating without it.
            log_warn("ragdoll: '%s' has no length; leaving it rigid",
                     skeleton->bones[bone].name ? skeleton->bones[bone].name : "?");
            continue;
        }

        float radius = measured_radius(mesh, bone, length);
        if (!(radius > 0.0f)) {
            radius = length * RAGDOLL_FALLBACK_RATIO;
        }
        // The capsule's cylinder, with the two hemispherical caps taken out of
        // the bone's length so the whole capsule spans the limb rather than
        // overhanging both joints by a radius.
        float half_height = length * 0.5f - radius;
        if (half_height < RAGDOLL_MIN_LENGTH) {
            // A bone shorter than it is thick -- a head, usually. Keep it a
            // capsule rather than refusing, by thinning it until it fits.
            radius = length * 0.4f;
            half_height = length * 0.5f - radius;
        }

        rd->parts[i].bone = bone;
        rd->parts[i].radius = radius * node_scale;
        rd->parts[i].half_height = half_height * node_scale;

        // The body sits at the MIDDLE of the limb with its own Y along it; the
        // bone sits at the head with whatever axes its author chose. This is
        // the transform between those two frames, in bone space, so the body's
        // world transform can be turned back into the bone's.
        vec3 dir;
        glm_vec3_divs(along, length, dir);
        mat4 bone_world;
        glm_mat4_copy(bind[bone], bone_world);
        vec3 mid;
        glm_vec3_lerp(joint, tail, 0.5f, mid);

        // A basis whose Y is the limb. The other two axes are arbitrary and
        // only have to be orthonormal -- a capsule is radially symmetric, so
        // nothing downstream can tell which way they point.
        vec3 up = {0.0f, 1.0f, 0.0f};
        if (fabsf(glm_vec3_dot(dir, up)) > 0.99f) {
            glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, up);
        }
        vec3 x_axis, z_axis;
        glm_vec3_cross(up, dir, x_axis);
        glm_vec3_normalize(x_axis);
        glm_vec3_cross(dir, x_axis, z_axis);
        glm_vec3_normalize(z_axis);

        mat4 body_world;
        glm_mat4_identity(body_world);
        glm_vec3_copy(x_axis, body_world[0]);
        glm_vec3_copy(dir, body_world[1]);
        glm_vec3_copy(z_axis, body_world[2]);
        glm_vec3_copy(mid, body_world[3]);

        // bone_from_body, kept the other way round for the apply: given a body
        // world transform, this recovers the bone's.
        mat4 body_inv;
        glm_mat4_inv(body_world, body_inv);
        glm_mat4_mul(body_inv, bone_world, rd->parts[i].body_from_bone);
    }

    free(bind);

    // Parents resolved after the collapse, so a missing chest re-points the
    // arms and the head at the spine rather than at nothing.
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        if (rd->parts[i].bone < 0) {
            continue;
        }
        if (i == RAGDOLL_HIPS) {
            rd->parts[i].parent = -1;
            continue;
        }
        int slot = (int)wanted[i].parent_slot;
        while (slot >= 0 && rd->parts[slot].bone < 0) {
            slot = (slot == RAGDOLL_HIPS) ? -1 : (int)wanted[slot].parent_slot;
        }
        rd->parts[i].parent = slot;
    }

    rd->count = 0;
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        if (rd->parts[i].bone >= 0) {
            rd->count++;
        }
    }

    // Refused rather than built with a hole. Arms bolted to nothing simulate
    // perfectly and read as a solver bug.
    if (rd->parts[RAGDOLL_HIPS].bone < 0 || rd->parts[RAGDOLL_THIGH_L].bone < 0 ||
        rd->parts[RAGDOLL_THIGH_R].bone < 0) {
        log_error("ragdoll: '%s' is missing a hip or a thigh; not a humanoid this can build",
                  skeleton->name ? skeleton->name : "?");
        free(rd);
        return NULL;
    }

    log_info("ragdoll: %d bodies from '%s' at scale %.3f", rd->count,
             skeleton->name ? skeleton->name : "?", (double)node_scale);
    return rd;
}

void free_ragdoll(RagdollSystem* ragdoll) {
    if (!ragdoll) {
        return;
    }
    if (ragdoll->jolt) {
        jolt_ragdoll_destroy(ragdoll->jolt);
    }
    free(ragdoll);
}

int ragdoll_bone_index(const RagdollSystem* ragdoll, RagdollBone which) {
    if (!ragdoll || which < 0 || which >= RAGDOLL_BONE_COUNT) {
        return -1;
    }
    return ragdoll->parts[which].bone;
}

bool ragdoll_capsule(const RagdollSystem* ragdoll, RagdollBone which, float* out_radius,
                     float* out_half_height) {
    if (!ragdoll || which < 0 || which >= RAGDOLL_BONE_COUNT || ragdoll->parts[which].bone < 0) {
        return false;
    }
    if (out_radius) {
        *out_radius = ragdoll->parts[which].radius;
    }
    if (out_half_height) {
        *out_half_height = ragdoll->parts[which].half_height;
    }
    return true;
}

bool ragdoll_active(const RagdollSystem* ragdoll) {
    return ragdoll && ragdoll->active;
}

void ragdoll_set_world(RagdollSystem* ragdoll, const mat4 to_world) {
    if (!ragdoll || !to_world) {
        return;
    }
    glm_mat4_copy((vec4*)to_world, ragdoll->to_world);
    glm_mat4_inv(ragdoll->to_world, ragdoll->to_model);
}

bool ragdoll_start(RagdollSystem* ragdoll, struct PhysicsWorld* physics, const mat4* globals,
                   const mat4 to_world) {
    if (!ragdoll || !physics || !globals || ragdoll->active) {
        return false;
    }
    ragdoll_set_world(ragdoll, to_world);

    RagdollBuild build[RAGDOLL_BONE_COUNT];
    memset(build, 0, sizeof(build));
    int build_index[RAGDOLL_BONE_COUNT];
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        build_index[i] = -1;
    }

    int n = 0;
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const RagdollPart* part = &ragdoll->parts[i];
        if (part->bone < 0) {
            continue;
        }
        build_index[i] = n;

        // The body's world transform from the LIVE pose, so the ragdoll
        // continues from where the character was rather than snapping to bind.
        mat4 bone_world, body_from_bone_inv, body_world;
        glm_mat4_mul((vec4*)ragdoll->to_world, (vec4*)globals[part->bone], bone_world);
        glm_mat4_inv((vec4*)part->body_from_bone, body_from_bone_inv);
        glm_mat4_mul(bone_world, body_from_bone_inv, body_world);

        build[n].name = ragdoll->skeleton->bones[part->bone].name;
        build[n].parent = (part->parent >= 0) ? build_index[part->parent] : -1;
        build[n].capsule_radius = part->radius;
        build[n].capsule_half_height = part->half_height;
        glm_mat4_copy(body_world, build[n].world);
        // Limits chosen per joint kind rather than per rig. A knee is nearly a
        // hinge and a shoulder is nearly free; everything else sits between.
        // These are authored numbers and Jolt's own sample says as much about
        // its equivalents -- a knee is not symmetric and this pretends it is.
        switch (i) {
            case RAGDOLL_SHIN_L:
            case RAGDOLL_SHIN_R:
                build[n].cone_deg = 5.0f;
                build[n].plane_deg = 75.0f;
                build[n].twist_min_deg = -5.0f;
                build[n].twist_max_deg = 5.0f;
                break;
            case RAGDOLL_LOWER_ARM_L:
            case RAGDOLL_LOWER_ARM_R:
                build[n].cone_deg = 5.0f;
                build[n].plane_deg = 80.0f;
                build[n].twist_min_deg = -20.0f;
                build[n].twist_max_deg = 20.0f;
                break;
            case RAGDOLL_UPPER_ARM_L:
            case RAGDOLL_UPPER_ARM_R:
                build[n].cone_deg = 70.0f;
                build[n].plane_deg = 70.0f;
                build[n].twist_min_deg = -45.0f;
                build[n].twist_max_deg = 45.0f;
                break;
            case RAGDOLL_HEAD:
                build[n].cone_deg = 30.0f;
                build[n].plane_deg = 30.0f;
                build[n].twist_min_deg = -45.0f;
                build[n].twist_max_deg = 45.0f;
                break;
            default: // spine, chest, thighs
                build[n].cone_deg = 35.0f;
                build[n].plane_deg = 35.0f;
                build[n].twist_min_deg = -25.0f;
                build[n].twist_max_deg = 25.0f;
                break;
        }
        n++;
    }

    if (n < 2) {
        log_error("ragdoll: %d body/bodies is not a ragdoll", n);
        return false;
    }

    // Group ids must be unique per ragdoll in one world, or two ragdolls stop
    // colliding with each other's limbs. Counted rather than derived from a
    // pointer, which would repeat on a reused allocation.
    static uint32_t next_group = 1;
    ragdoll->jolt = jolt_ragdoll_create(physics->physics_system, build, n, next_group++);
    if (!ragdoll->jolt) {
        return false;
    }
    ragdoll->active = true;
    return true;
}

void ragdoll_apply(RagdollSystem* ragdoll, mat4* global_transforms, size_t bone_count) {
    if (!ragdoll || !ragdoll->active || !ragdoll->jolt || !global_transforms) {
        return;
    }

    // Simulated bones first, from their bodies.
    int n = 0;
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const RagdollPart* part = &ragdoll->parts[i];
        if (part->bone < 0) {
            continue;
        }
        // Identity rather than undefined: the getter leaves it untouched when it
        // refuses, and a caller reading a refusal would otherwise read the
        // stack.
        mat4 body_world = GLM_MAT4_IDENTITY_INIT;
        if (jolt_ragdoll_get_world(ragdoll->jolt, n, body_world)) {
            mat4 bone_world, bone_model;
            glm_mat4_mul(body_world, (vec4*)part->body_from_bone, bone_world);
            glm_mat4_mul(ragdoll->to_model, bone_world, bone_model);
            // The node scale has to come back OUT of the basis. to_model
            // carries 1/scale and a Jolt body's transform carries none, so
            // their product is a bone global shrunk by the node's scale --
            // which skins a character at half size with its limbs at half their
            // offsets, and reads as the ragdoll having exploded into confetti
            // rather than as a scale bug. The translation keeps the division,
            // because that one is a real change of units.
            for (int c = 0; c < 3; c++) {
                glm_vec3_normalize(bone_model[c]);
            }
            if ((size_t)part->bone < bone_count) {
                glm_mat4_copy(bone_model, global_transforms[part->bone]);
            }
        }
        n++;
    }

    // Then everything else, re-accumulated from its parent using its BIND
    // local. A hand keeps its shape on the end of a forearm, and a finger keeps
    // its shape on the end of a hand -- parent-first ordering is what makes one
    // pass enough.
    uint8_t simulated[256];
    memset(simulated, 0, sizeof(simulated));
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const int bone = ragdoll->parts[i].bone;
        if (bone >= 0 && bone < (int)sizeof(simulated)) {
            simulated[bone] = 1;
        }
    }
    const Skeleton* skeleton = ragdoll->skeleton;
    for (size_t i = 0; i < skeleton->bone_count && i < bone_count; i++) {
        if (i < sizeof(simulated) && simulated[i]) {
            continue;
        }
        const int parent = skeleton->bones[i].parent_index;
        if (parent < 0 || (size_t)parent >= bone_count) {
            continue;
        }
        glm_mat4_mul(global_transforms[parent], (vec4*)skeleton->bones[i].local_transform,
                     global_transforms[i]);
    }
}

bool ragdoll_hips_world(const RagdollSystem* ragdoll, vec3 out) {
    if (!ragdoll || !ragdoll->active || !ragdoll->jolt || !out) {
        return false;
    }
    mat4 body_world = GLM_MAT4_IDENTITY_INIT;
    if (!jolt_ragdoll_get_world(ragdoll->jolt, 0, body_world)) {
        return false;
    }
    glm_vec3_copy(body_world[3], out);
    return true;
}
