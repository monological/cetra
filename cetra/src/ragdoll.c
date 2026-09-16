#include "ragdoll.h"

#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "mesh.h"
#include "rigging.h"

// A radius for a bone the mesh could not measure, as a fraction of its own
// length. Thin enough to read as a limb and never zero, which CapsuleShape
// asserts on.
#define RAGDOLL_FALLBACK_RATIO 0.18f
// And for a bone shorter than it is thick -- a head, usually -- where the
// measured radius leaves no cylinder between the two caps.
#define RAGDOLL_STUBBY_RATIO 0.40f
// Below this a bone is not a limb. A zero-length bone is a rig's control or
// twist joint; building a capsule for one is what the Jolt layer refuses.
#define RAGDOLL_MIN_LENGTH 1e-4f

/*
 * ONE description of a humanoid, and everything in it is a compile-time
 * constant. Only which SKELETON bone answers each row varies by rig, which is
 * what `RagdollPart.bone` holds.
 *
 * The joint limits live here rather than in a switch at build time because this
 * is the table someone tunes: `docs/verification.md` records that no arm can
 * say whether a settled heap looks like a person, so the limits are the first
 * thing to reach for if a character ever reads as rubbery. They should be
 * beside the bones they bend.
 *
 * They are authored numbers, and approximate ones -- a knee gets a narrow cone
 * and a wide plane, which is a hinge pretending to be symmetric. Jolt's own
 * sample says the same of its equivalents.
 */
typedef struct RagdollBoneDef {
    const char* name; // the slot's own name, not the rig's
    int parent;       // slot this hangs from; -1 for the root
    // How the skeleton bone is found. A NAME goes through the semantic
    // resolver; a CATEGORY is walked, because some categories cover a whole
    // chain and the resolver refuses ambiguity by design (see below).
    const char* resolve_name;
    BoneCategory category;
    int under;     // slot the walk descends from; unused when resolve_name is set
    bool furthest; // take the furthest of the category rather than the nearest
    float cone_deg, plane_deg, twist_min_deg, twist_max_deg;
} RagdollBoneDef;

static const RagdollBoneDef RAGDOLL_HUMANOID[RAGDOLL_BONE_COUNT] = {
    [RAGDOLL_HIPS] = {"hips", -1, "hips", BCAT_UNKNOWN, -1, false, 35, 35, -25, 25},
    [RAGDOLL_SPINE] = {"spine", RAGDOLL_HIPS, NULL, BCAT_SPINE, RAGDOLL_HIPS, false, 35, 35, -25,
                       25},
    [RAGDOLL_CHEST] = {"chest", RAGDOLL_SPINE, NULL, BCAT_SPINE, RAGDOLL_HIPS, true, 35, 35, -25,
                       25},
    [RAGDOLL_HEAD] = {"head", RAGDOLL_CHEST, NULL, BCAT_HEAD, RAGDOLL_CHEST, false, 30, 30, -45,
                      45},
    [RAGDOLL_UPPER_ARM_L] = {"arm_l", RAGDOLL_CHEST, "leftarm", BCAT_UNKNOWN, -1, false, 70, 70,
                             -45, 45},
    [RAGDOLL_LOWER_ARM_L] = {"forearm_l", RAGDOLL_UPPER_ARM_L, "leftforearm", BCAT_UNKNOWN, -1,
                             false, 5, 80, -20, 20},
    [RAGDOLL_UPPER_ARM_R] = {"arm_r", RAGDOLL_CHEST, "rightarm", BCAT_UNKNOWN, -1, false, 70, 70,
                             -45, 45},
    [RAGDOLL_LOWER_ARM_R] = {"forearm_r", RAGDOLL_UPPER_ARM_R, "rightforearm", BCAT_UNKNOWN, -1,
                             false, 5, 80, -20, 20},
    [RAGDOLL_THIGH_L] = {"thigh_l", RAGDOLL_HIPS, "leftupleg", BCAT_UNKNOWN, -1, false, 35, 35, -25,
                         25},
    [RAGDOLL_SHIN_L] = {"shin_l", RAGDOLL_THIGH_L, "leftleg", BCAT_UNKNOWN, -1, false, 5, 75, -5,
                        5},
    [RAGDOLL_THIGH_R] = {"thigh_r", RAGDOLL_HIPS, "rightupleg", BCAT_UNKNOWN, -1, false, 35, 35,
                         -25, 25},
    [RAGDOLL_SHIN_R] = {"shin_r", RAGDOLL_THIGH_R, "rightleg", BCAT_UNKNOWN, -1, false, 5, 75, -5,
                        5},
};

typedef struct RagdollPart {
    int bone;   // index into the skeleton, -1 if unresolved
    int parent; // slot this hangs from after unresolved rows collapse out
    int body;   // index into the Jolt ragdoll's bodies, -1 until started
    float radius;
    float half_height;
    // The bind transform taking the BONE's frame to the capsule BODY's, and its
    // inverse. Both stored because both are build-time constants and each is
    // wanted in one direction: start needs body-from-bone to place a body from
    // a posed bone, apply needs bone-from-body to do the reverse.
    mat4 bone_to_body;
    mat4 body_to_bone;
} RagdollPart;

struct RagdollSystem {
    Skeleton* skeleton; // borrowed
    RagdollPart parts[RAGDOLL_BONE_COUNT];
    // Bone index to the slot driving it, -1 for a bone the ragdoll does not
    // simulate. springbone.h's `bone_to_joint` shape, and for its reason: the
    // apply is one pass over the skeleton, and this is what lets it be.
    int bone_to_slot[MAX_BONES];

    JoltRagdoll* jolt; // non-NULL IS the active state; there is no second flag
    mat4 to_model;
};

const char* ragdoll_bone_name(RagdollBone which) {
    if (which < 0 || which >= RAGDOLL_BONE_COUNT) {
        return NULL;
    }
    return RAGDOLL_HUMANOID[which].name;
}

int ragdoll_bone_parent(const RagdollSystem* ragdoll, RagdollBone which) {
    if (!ragdoll || which < 0 || which >= RAGDOLL_BONE_COUNT) {
        return -1;
    }
    return ragdoll->parts[which].parent;
}

/*
 * Some bones cannot be found by NAME, and that is a property of the vocabulary
 * rather than an accident of one rig.
 *
 * categorize_bone gives one category to a whole chain -- BCAT_SPINE covers
 * "spine" and "chest" alike, BCAT_HEAD covers "Head" and "HeadTop_End" -- and
 * the semantic matcher answers only when EXACTLY ONE bone matches, so on a
 * segmented spine or a Mixamo head it correctly refuses to pick. Asking it for
 * "the chest" is asking a question the name does not answer. get_bone_position
 * does disambiguate a spine, but by MIXAMO NAME PATTERN, so a rig spelling the
 * chain Spine/Chest/UpperChest gets a different answer than Spine/Spine1/Spine2.
 *
 * The hierarchy always knows. This asks it: which bones of this category
 * descend from that ancestor, and which of them is nearest or furthest.
 * Parent-first ordering makes the first found nearest and the last furthest,
 * with no depth arithmetic.
 */
static int resolve_by_category(const Skeleton* skeleton, int ancestor, BoneCategory want,
                               bool furthest, uint8_t* scratch) {
    if (ancestor < 0) {
        return -1;
    }
    // The descendant test through the shared walk rather than a fourth hand-
    // rolled parent chase: skeleton_mark_subtree carries the parent-first guard
    // that a hand-rolled loop is exactly the place to forget.
    skeleton_mark_subtree(skeleton, ancestor, scratch);
    scratch[ancestor] = 0; // strict descendants; the ancestor is not its own

    int found = -1;
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        if (!scratch[i]) {
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
        found = (int)i;
        if (!furthest) {
            break;
        }
    }
    return found;
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

/*
 * The capsule for one bone, all three rules in one place.
 *
 * The radius is measured from the mesh's own per-bone bind box where there is
 * one: the limb runs along that box's LONGEST extent, so its cross-section is
 * the other two and the radius is their mean. Chosen by value rather than by
 * axis, because a bone's local axes are the rig author's and not a convention
 * this can rely on -- dropping the largest is the whole rule.
 *
 * Two fallbacks, and each answers a different failure. A bone the mesh cannot
 * measure takes a fraction of its own length. A bone shorter than it is thick
 * is thinned until a cylinder fits between the two caps, because the caps alone
 * are a sphere and CapsuleShape wants a positive half height.
 */
static void capsule_for_bone(const struct Mesh* mesh, int bone, float length, float* out_radius,
                             float* out_half_height) {
    float radius = 0.0f;
    if (mesh && mesh->bone_aabb && bone >= 0 && (size_t)bone < mesh->bone_aabb_count) {
        const AABB box = mesh->bone_aabb[bone];
        if (!aabb_is_empty(&box)) {
            const float ex = (box.max[0] - box.min[0]) * 0.5f;
            const float ey = (box.max[1] - box.min[1]) * 0.5f;
            const float ez = (box.max[2] - box.min[2]) * 0.5f;
            float longest = ex;
            if (ey > longest) {
                longest = ey;
            }
            if (ez > longest) {
                longest = ez;
            }
            radius = (ex + ey + ez - longest) * 0.5f;
        }
    }
    if (!(radius > 0.0f)) {
        radius = length * RAGDOLL_FALLBACK_RATIO;
    }
    float half_height = length * 0.5f - radius;
    if (half_height < RAGDOLL_MIN_LENGTH) {
        radius = length * RAGDOLL_STUBBY_RATIO;
        half_height = length * 0.5f - radius;
    }
    *out_radius = radius;
    *out_half_height = half_height;
}

// Which skeleton bone answers each row of the humanoid, or -1. The one part of
// the description that varies by rig.
static void resolve_bones(Skeleton* skeleton, int* out_bone) {
    uint8_t scratch[MAX_BONES];
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const RagdollBoneDef* def = &RAGDOLL_HUMANOID[i];
        if (def->resolve_name) {
            out_bone[i] = skeleton_resolve_bone(skeleton, def->resolve_name);
        } else {
            out_bone[i] = resolve_by_category(skeleton, out_bone[def->under], def->category,
                                              def->furthest, scratch);
        }
    }
    // A one-segment spine answers both spine rows with the same bone. Dropping
    // the chest rather than building two bodies on one bone is what makes the
    // body count a property of the rig instead of a constant.
    if (out_bone[RAGDOLL_CHEST] == out_bone[RAGDOLL_SPINE]) {
        out_bone[RAGDOLL_CHEST] = -1;
    }
}

// The capsule and the two frame transforms for every resolved row.
static void measure_capsules(const Skeleton* skeleton, const struct Mesh* mesh, const mat4* bind,
                             float node_scale, RagdollPart* parts) {
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const int bone = parts[i].bone;
        if (bone < 0) {
            continue;
        }

        // The limb's own length, from its bind head to its child's. A tip bone
        // has no child and takes its parent's separation instead, which is the
        // only measurement of it the rig carries.
        const int child = first_child(skeleton, bone);
        vec3 joint, tail;
        glm_vec3_copy((float*)bind[bone][3], joint);
        if (child >= 0) {
            glm_vec3_copy((float*)bind[child][3], tail);
        } else {
            const int parent = skeleton->bones[bone].parent_index;
            if (parent >= 0) {
                vec3 back;
                glm_vec3_sub(joint, (float*)bind[parent][3], back);
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
            parts[i].bone = -1;
            continue;
        }

        float radius = 0.0f, half_height = 0.0f;
        capsule_for_bone(mesh, bone, length, &radius, &half_height);
        parts[i].radius = radius * node_scale;
        parts[i].half_height = half_height * node_scale;

        // The body sits at the MIDDLE of the limb with its own Y along it; the
        // bone sits at the head with whatever axes its author chose. These two
        // matrices are the change between those frames, in bind space.
        vec3 dir;
        glm_vec3_divs(along, length, dir);
        vec3 mid;
        glm_vec3_lerp(joint, tail, 0.5f, mid);

        // A basis whose Y is the limb. The other two axes only have to be
        // orthonormal -- a capsule is radially symmetric, so nothing downstream
        // can tell which way they point.
        vec3 x_axis, z_axis;
        glm_vec3_ortho(dir, x_axis);
        glm_vec3_normalize(x_axis);
        glm_vec3_cross(dir, x_axis, z_axis);
        glm_vec3_normalize(z_axis);

        mat4 body_world;
        glm_mat4_identity(body_world);
        glm_vec3_copy(x_axis, body_world[0]);
        glm_vec3_copy(dir, body_world[1]);
        glm_vec3_copy(z_axis, body_world[2]);
        glm_vec3_copy(mid, body_world[3]);

        mat4 body_inv;
        glm_mat4_inv(body_world, body_inv);
        glm_mat4_mul(body_inv, (vec4*)bind[bone], parts[i].bone_to_body);
        glm_mat4_inv(parts[i].bone_to_body, parts[i].body_to_bone);
    }
}

// The slot each row hangs from once unresolved rows have collapsed out, so a
// missing chest re-points the arms and the head at the spine rather than at
// nothing.
static void link_parents(RagdollPart* parts) {
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        if (parts[i].bone < 0) {
            parts[i].parent = -1;
            continue;
        }
        int slot = RAGDOLL_HUMANOID[i].parent;
        while (slot >= 0 && parts[slot].bone < 0) {
            slot = RAGDOLL_HUMANOID[slot].parent;
        }
        parts[i].parent = slot;
    }
}

RagdollSystem* create_ragdoll(Skeleton* skeleton, const struct Mesh* mesh, float node_scale) {
    if (!skeleton || skeleton->bone_count == 0) {
        log_error("ragdoll: no skeleton");
        return NULL;
    }
    if (skeleton->bone_count > MAX_BONES) {
        log_error("ragdoll: '%s' has %zu bones, past the %d a pose can carry",
                  skeleton->name ? skeleton->name : "?", skeleton->bone_count, MAX_BONES);
        return NULL;
    }
    if (!(node_scale > 0.0f)) {
        log_error("ragdoll: node scale %.6f is not positive", (double)node_scale);
        return NULL;
    }

    RagdollSystem* rd = calloc(1, sizeof(RagdollSystem));
    if (!rd) {
        log_error("ragdoll: out of memory");
        return NULL;
    }
    rd->skeleton = skeleton;
    glm_mat4_identity(rd->to_model);

    int bone[RAGDOLL_BONE_COUNT];
    resolve_bones(skeleton, bone);
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        rd->parts[i].bone = bone[i];
        rd->parts[i].parent = -1;
        rd->parts[i].body = -1;
    }

    mat4* bind = calloc(skeleton->bone_count, sizeof(mat4));
    if (!bind) {
        log_error("ragdoll: out of memory");
        free(rd);
        return NULL;
    }
    skeleton_compute_bind_globals(skeleton, bind);
    measure_capsules(skeleton, mesh, bind, node_scale, rd->parts);
    free(bind);

    link_parents(rd->parts);

    // Refused rather than built with a hole. Arms bolted to nothing simulate
    // perfectly and read as a solver bug.
    if (rd->parts[RAGDOLL_HIPS].bone < 0 || rd->parts[RAGDOLL_THIGH_L].bone < 0 ||
        rd->parts[RAGDOLL_THIGH_R].bone < 0) {
        log_error("ragdoll: '%s' is missing a hip or a thigh; not a humanoid this can build",
                  skeleton->name ? skeleton->name : "?");
        free(rd);
        return NULL;
    }

    for (size_t i = 0; i < MAX_BONES; i++) {
        rd->bone_to_slot[i] = -1;
    }
    int bodies = 0;
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        if (rd->parts[i].bone >= 0) {
            rd->bone_to_slot[rd->parts[i].bone] = i;
            bodies++;
        }
    }

    log_info("ragdoll: %d bodies from '%s' at scale %.3f", bodies,
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
    return ragdoll && ragdoll->jolt;
}

void ragdoll_set_world(RagdollSystem* ragdoll, const mat4 to_world) {
    if (!ragdoll || !to_world) {
        return;
    }
    glm_mat4_inv((vec4*)to_world, ragdoll->to_model);
}

bool ragdoll_start(RagdollSystem* ragdoll, JPC_PhysicsSystem* system, uint32_t object_layer,
                   const mat4* globals, const mat4 to_world) {
    if (!ragdoll || !system || !globals || ragdoll->jolt) {
        return false;
    }
    ragdoll_set_world(ragdoll, to_world);

    RagdollBuild build[RAGDOLL_BONE_COUNT];
    memset(build, 0, sizeof(build));

    int n = 0;
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        RagdollPart* part = &ragdoll->parts[i];
        if (part->bone < 0) {
            continue;
        }
        // Stored rather than recomputed at apply: the slot-to-body numbering is
        // an invariant two functions would otherwise have to derive identically
        // from the same filter, which survives one edit and not two.
        part->body = n;

        // The body's world transform from the LIVE pose, so the ragdoll
        // continues from where the character was rather than snapping to bind.
        mat4 bone_world;
        glm_mat4_mul((vec4*)to_world, (vec4*)globals[part->bone], bone_world);
        glm_mat4_mul(bone_world, part->body_to_bone, build[n].world);

        const RagdollBoneDef* def = &RAGDOLL_HUMANOID[i];
        build[n].name = ragdoll->skeleton->bones[part->bone].name;
        build[n].parent = (part->parent >= 0) ? ragdoll->parts[part->parent].body : -1;
        build[n].capsule_radius = part->radius;
        build[n].capsule_half_height = part->half_height;
        build[n].cone_deg = def->cone_deg;
        build[n].plane_deg = def->plane_deg;
        build[n].twist_min_deg = def->twist_min_deg;
        build[n].twist_max_deg = def->twist_max_deg;
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
    ragdoll->jolt = jolt_ragdoll_create(system, build, n, object_layer, next_group++);
    return ragdoll->jolt != NULL;
}

void ragdoll_apply(RagdollSystem* ragdoll, mat4* global_transforms) {
    if (!ragdoll || !ragdoll->jolt || !global_transforms) {
        return;
    }

    // One pass, parent-first. A simulated bone takes its body's transform;
    // every other bone re-accumulates from its parent using its BIND local, so
    // a hand keeps its shape on the end of a forearm and a finger on the end of
    // a hand. Parent-first ordering is what makes one pass enough.
    const Skeleton* skeleton = ragdoll->skeleton;
    for (size_t i = 0; i < skeleton->bone_count; i++) {
        const int slot = ragdoll->bone_to_slot[i];
        if (slot >= 0) {
            mat4 body_world = GLM_MAT4_IDENTITY_INIT;
            if (!jolt_ragdoll_get_world(ragdoll->jolt, ragdoll->parts[slot].body, body_world)) {
                continue;
            }
            mat4 bone_world;
            glm_mat4_mul(body_world, ragdoll->parts[slot].bone_to_body, bone_world);
            glm_mat4_mul(ragdoll->to_model, bone_world, global_transforms[i]);
            // The node scale has to come back OUT of the basis. to_model
            // carries 1/scale and a Jolt body's transform carries none, so
            // their product is a bone global shrunk by the node's scale --
            // which skins a character at half size with its limbs at half their
            // offsets, and reads as the ragdoll having exploded into confetti
            // rather than as a scale bug. The translation keeps the division,
            // because that one is a real change of units.
            for (int c = 0; c < 3; c++) {
                glm_vec3_normalize(global_transforms[i][c]);
            }
            continue;
        }
        const int parent = skeleton->bones[i].parent_index;
        if (parent >= 0) {
            glm_mat4_mul(global_transforms[parent], (vec4*)skeleton->bones[i].local_transform,
                         global_transforms[i]);
        }
    }
}

bool ragdoll_hips_world(const RagdollSystem* ragdoll, vec3 out) {
    if (!ragdoll || !ragdoll->jolt || !out) {
        return false;
    }
    mat4 body_world = GLM_MAT4_IDENTITY_INIT;
    if (!jolt_ragdoll_get_world(ragdoll->jolt, ragdoll->parts[RAGDOLL_HIPS].body, body_world)) {
        return false;
    }
    glm_vec3_copy(body_world[3], out);
    return true;
}
