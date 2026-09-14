#include "ik.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"

// A leg at full extension puts acos's argument exactly on -1, and one ulp past it is
// NaN. That NaN would reach bone_matrices, and from there prev_bone_rows, so it
// poisons the FOLLOWING frame's motion vectors as well as this one's picture. Two
// clamps are carried rather than one because they bound different things: the
// distance clamp bounds the geometry a caller asked for, the cosine clamp bounds the
// rounding of the division that follows.
#define IK_EPS       1e-6f // a length below which a segment has no direction
#define IK_DIR_EPS   1e-8f // the same for a cross product, which squares the error
#define IK_REACH_PAD 1e-4f // held off the inner limit, where the knee folds back on itself

IkFootParams ik_default_params(void) {
    IkFootParams p;
    // 0.61 of a leg, which on this puppet's 0.82 is the 0.5 m this was before it became
    // a fraction -- chosen to preserve the behaviour rather than to retune it. As a
    // fraction it carries to a rig of another size, which a metre never did.
    p.max_pelvis_drop = 0.6098f;
    p.teleport_distance = 0.5f;
    p.blend_rate = 12.0f;
    // A tenth of the leg: on a rig whose thigh and shin sum to 0.82 that is 0.082, and
    // this puppet's walk carries the ankle about 0.15 over a stride -- so a foot is
    // fully released around halfway up its swing and planted again near contact.
    p.plant_fraction = 0.10f;
    // A toe within 5 per cent of a leg of the ground and moving up or down slower than
    // a quarter of a leg per second is down. On this puppet that is 0.041 m and 0.205
    // m/s, against a walk whose toe lifts 0.165 m in about a fifth of a second -- so a
    // swinging foot clears both by a wide margin and neither is a hair trigger.
    p.contact_height = 0.05f;
    p.contact_speed = 0.25f;
    return p;
}

IkSystem* create_ik_system(Skeleton* skeleton) {
    if (!skeleton) {
        log_error("create_ik_system: no skeleton");
        return NULL;
    }
    IkSystem* system = calloc(1, sizeof(IkSystem));
    if (!system) {
        log_error("create_ik_system: out of memory");
        return NULL;
    }
    system->skeleton = skeleton;
    system->pelvis_index = -1;
    system->params = ik_default_params();
    system->enabled = true;
    system->needs_reset = true;
    return system;
}

void free_ik_system(IkSystem* system) {
    if (!system)
        return;
    free(system->feet);
    free(system);
}

int ik_add_foot(IkSystem* system, const char* hip_bone, const char* knee_bone,
                const char* ankle_bone, const vec3 knee_forward) {
    if (!system || !hip_bone || !knee_bone || !ankle_bone) {
        log_error("ik_add_foot: a null argument");
        return -1;
    }

    Skeleton* skeleton = system->skeleton;
    // The lookup reads the map and declares the skeleton non-const.
    const int hip = get_bone_index_by_name(skeleton, hip_bone);
    const int knee = get_bone_index_by_name(skeleton, knee_bone);
    const int ankle = get_bone_index_by_name(skeleton, ankle_bone);
    if (hip < 0 || knee < 0 || ankle < 0) {
        log_error("ik_add_foot: '%s' has no bone named '%s'", skeleton->name,
                  hip < 0 ? hip_bone : (knee < 0 ? knee_bone : ankle_bone));
        return -1;
    }
    if (skeleton->bones[knee].parent_index != hip || skeleton->bones[ankle].parent_index != knee) {
        log_error("ik_add_foot: '%s' -> '%s' -> '%s' is not a parent chain", hip_bone, knee_bone,
                  ankle_bone);
        return -1;
    }

    IkFoot* grown = realloc(system->feet, (system->foot_count + 1) * sizeof(IkFoot));
    if (!grown) {
        log_error("ik_add_foot: out of memory");
        return -1;
    }
    system->feet = grown;

    IkFoot* foot = &system->feet[system->foot_count];
    memset(foot, 0, sizeof(*foot));
    foot->hip_index = hip;
    foot->knee_index = knee;
    foot->ankle_index = ankle;
    // Judged at the ankle until a toe is registered. Not -1: every reader would then
    // carry the same two-branch test, and a rig without a toe is not a rig without a
    // contact.
    foot->toe_index = ankle;

    glm_vec3_copy((float*)knee_forward, foot->pole_local);
    if (glm_vec3_norm(foot->pole_local) < IK_DIR_EPS)
        glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, foot->pole_local);
    glm_vec3_normalize(foot->pole_local);

    // The axis a leg bends about when it is aimed straight ALONG the pole, which is
    // the one direction the pole cannot resolve. Taken from the bind pose, where the
    // chain's plane is whatever the rig author meant by it.
    glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, foot->fallback_axis);
    bool axis_derived = false;
    mat4* bind = calloc(skeleton->bone_count, sizeof(mat4));
    if (bind) {
        skeleton_compute_bind_globals(skeleton, bind);
        vec3 a, c, limb, axis;
        glm_vec3_copy(bind[hip][3], a);
        glm_vec3_copy(bind[ankle][3], c);
        // The ankle's own height at bind IS its clearance above the sole, on any rig
        // whose bind sole rests at model y = 0. Free here: the bind globals are already
        // built for the axis below.
        foot->sole_offset = c[1];
        foot->toe_offset = c[1]; // the toe IS the ankle until ik_foot_set_toe says otherwise
        glm_vec3_sub(c, a, limb);
        if (glm_vec3_norm(limb) > IK_EPS) {
            glm_vec3_normalize(limb);
            glm_vec3_cross(limb, foot->pole_local, axis);
            if (glm_vec3_norm(axis) > IK_DIR_EPS) {
                glm_vec3_normalize(axis);
                glm_vec3_copy(axis, foot->fallback_axis);
                axis_derived = true;
            }
        }
        free(bind);
    } else {
        // Refused rather than half-registered. sole_offset comes from these globals too,
        // so a foot that survived this would plant into the floor by its own thickness
        // for the rest of the run -- under a log line that talks about the bend axis.
        log_error("ik_add_foot: out of memory");
        return -1;
    }
    // Said out loud rather than guessed in silence. This axis decides WHICH WAY the
    // knee bends in the one configuration the pole cannot resolve -- and on a rig whose
    // bind pose is exactly that configuration, an arbitrary (1,0,0) is a knee bending
    // sideways for no reason the caller can see.
    if (!axis_derived)
        log_error("ik_add_foot: '%s' -> '%s' -> '%s' gives no bend axis at bind; "
                  "falling back to +X, which may bend the knee sideways",
                  hip_bone, knee_bone, ankle_bone);

    return (int)system->foot_count++;
}

bool ik_foot_set_toe(IkSystem* system, int foot, const char* toe_bone) {
    if (!system || !toe_bone || foot < 0 || (size_t)foot >= system->foot_count) {
        log_error("ik_foot_set_toe: a null argument or no such foot");
        return false;
    }
    Skeleton* skeleton = system->skeleton;
    const int toe = get_bone_index_by_name(skeleton, toe_bone);
    IkFoot* f = &system->feet[foot];
    if (toe < 0) {
        log_error("ik_foot_set_toe: '%s' has no bone named '%s'", skeleton->name, toe_bone);
        return false;
    }
    // A child of the ANKLE, not merely a descendant. A toe two joints down would still
    // ride the solve, but the ankle-to-toe offset the lock converts through is taken as
    // a rigid vector, and that is only true across one joint.
    if (skeleton->bones[toe].parent_index != f->ankle_index) {
        log_error("ik_foot_set_toe: '%s' is not a child of this foot's ankle", toe_bone);
        return false;
    }

    mat4* bind = calloc(skeleton->bone_count, sizeof(mat4));
    if (!bind) {
        log_error("ik_foot_set_toe: out of memory");
        return false;
    }
    skeleton_compute_bind_globals(skeleton, bind);
    // The same derivation sole_offset gets, on the same assumption: a bind sole at
    // model y = 0. A toe is thinner than an ankle is tall, so the two differ and using
    // the ankle's for both would call a toe in contact while it was still a heel's
    // height off the ground.
    f->toe_offset = bind[toe][3][1];
    free(bind);

    f->toe_index = toe;
    return true;
}

bool ik_set_pelvis(IkSystem* system, const char* pelvis_bone) {
    if (!system || !pelvis_bone) {
        log_error("ik_set_pelvis: a null argument");
        return false;
    }

    Skeleton* skeleton = system->skeleton;
    const int pelvis = get_bone_index_by_name(skeleton, pelvis_bone);
    if (pelvis < 0) {
        log_error("ik_set_pelvis: '%s' has no bone named '%s'", skeleton->name, pelvis_bone);
        return false;
    }

    skeleton_mark_subtree(skeleton, pelvis, system->in_pelvis_subtree);

    // Dropping a pelvis that does not carry every foot would lower half a body and
    // leave the rest standing, which reads as a broken rig rather than a short leg.
    for (size_t i = 0; i < system->foot_count; i++) {
        if (!system->in_pelvis_subtree[system->feet[i].ankle_index]) {
            log_error("ik_set_pelvis: '%s' is not an ancestor of every registered foot",
                      pelvis_bone);
            // All or nothing. Clearing the mask while leaving a previously accepted
            // pelvis_index set would arm the drop against a mask matching no bone: the
            // deficit is computed, the translation reaches nothing, and the caller was
            // told false about a call it may not have expected to matter.
            memset(system->in_pelvis_subtree, 0, sizeof(system->in_pelvis_subtree));
            system->pelvis_index = -1;
            return false;
        }
    }

    system->pelvis_index = pelvis;
    return true;
}

void ik_foot_set_target(IkSystem* system, int foot, const vec3 target, const vec3 normal,
                        float weight) {
    if (!system) {
        log_error("ik_foot_set_target: no system");
        return;
    }
    if (foot < 0 || (size_t)foot >= system->foot_count) {
        log_error("ik_foot_set_target: no foot %d", foot);
        return;
    }
    IkFoot* f = &system->feet[foot];
    glm_vec3_copy((float*)target, f->target);
    if (normal)
        glm_vec3_copy((float*)normal, f->normal);
    f->weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
}

void ik_foot_set_ground(IkSystem* system, int foot, const vec3 ground, const vec3 normal,
                        float weight) {
    if (!system) {
        log_error("ik_foot_set_ground: no system");
        return;
    }
    if (foot < 0 || (size_t)foot >= system->foot_count) {
        log_error("ik_foot_set_ground: no foot %d", foot);
        return;
    }
    vec3 at = {ground[0], ground[1] + system->feet[foot].sole_offset, ground[2]};
    ik_foot_set_target(system, foot, at, normal, weight);
}

void ik_reset(IkSystem* system) {
    if (!system) {
        log_error("ik_reset: no system");
        return;
    }
    system->needs_reset = true;
    // A teleport invalidates the pose the contact speed is differenced against: across
    // the jump the toe appears to move the whole distance in one frame, which reads as
    // a foot travelling at an enormous rate and suppresses the label for exactly one
    // frame after every load. Dropping the history says the truth instead, which is
    // that there is no previous pose to compare with.
    for (size_t i = 0; i < system->foot_count; i++)
        system->feet[i].has_clip_prev = false;
}

// The in-place form of skeleton_rotate_global, which is what every site here wants;
// naming it keeps the subscript off both sides of each call.
static void rotate_global(mat4 m, versor q, const vec3 head) {
    skeleton_rotate_global(m, m, q, head);
}

// Rotate hip and knee so the ankle lands on `target`. Model space throughout, and only
// this chain's three bones are written. A degenerate segment leaves the chain at its
// animated pose, the way a spring bone follows rigidly below its own epsilon.
static void solve_two_bone(mat4* g, const IkFoot* f, const vec3 target) {
    vec3 a, b, c;
    glm_vec3_copy(g[f->hip_index][3], a);
    glm_vec3_copy(g[f->knee_index][3], b);
    glm_vec3_copy(g[f->ankle_index][3], c);

    vec3 ab, bc;
    glm_vec3_sub(b, a, ab);
    glm_vec3_sub(c, b, bc);
    const float l1 = glm_vec3_norm(ab);
    const float l2 = glm_vec3_norm(bc);
    if (l1 < IK_EPS || l2 < IK_EPS)
        return;

    vec3 d;
    glm_vec3_sub((float*)target, a, d);
    const float want = glm_vec3_norm(d);
    if (want < IK_EPS)
        return; // the target sits on the hip: there is no direction to aim along

    vec3 dir;
    glm_vec3_normalize_to(d, dir);

    // Full extension, and deliberately not a knob. Shortening the reach to "keep a bend
    // off the singularity" is a trap: near full extension the knee angle goes as the
    // SQUARE ROOT of the shortening, so on a rig whose bind pose is exactly straight
    // 0.995 buys an unremovable 11.5 degrees -- the legs read as permanently bent and
    // swing from the hip alone. Numerical safety at the singularity is the cosine
    // clamp's job below, not this one's; a game that wants a soft knee bends the clip.
    float lo = fabsf(l1 - l2) * (1.0f + IK_REACH_PAD);
    float hi = l1 + l2;
    if (hi < lo)
        hi = lo;
    const float dist = glm_clamp(want, lo, hi);

    // The bend plane, and its ordering is the reverse of the textbook one. On a rig
    // whose bind pose is straight and whose locomotion never bends a knee, the current
    // triangle is degenerate on nearly every frame, so the POLE is the primary source
    // and the triangle normal the fallback. The pole rides the hip's own frame, so a
    // clip that turns the hips carries knee-forward around with it.
    vec3 pole, axis;
    glm_mat4_mulv3(g[f->hip_index], (float*)f->pole_local, 0.0f, pole);
    glm_vec3_cross(dir, pole, axis);
    if (glm_vec3_norm(axis) < IK_DIR_EPS) {
        vec3 ac;
        glm_vec3_sub(c, a, ac);
        glm_vec3_cross(ab, ac, axis);
    }
    if (glm_vec3_norm(axis) < IK_DIR_EPS)
        glm_vec3_copy((float*)f->fallback_axis, axis);
    if (glm_vec3_norm(axis) < IK_DIR_EPS)
        return;
    glm_vec3_normalize(axis);

    float cos_hip = (l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist);
    cos_hip = glm_clamp(cos_hip, -1.0f, 1.0f);

    vec3 knee_dir;
    glm_vec3_copy(dir, knee_dir);
    glm_vec3_rotate(knee_dir, acosf(cos_hip), axis);

    vec3 knee_new, ankle_new, scratch;
    glm_vec3_scale(knee_dir, l1, scratch);
    glm_vec3_add(a, scratch, knee_new);
    glm_vec3_scale(dir, dist, scratch);
    glm_vec3_add(a, scratch, ankle_new);

    // The thigh swings onto its new direction and carries the whole limb rigidly, so
    // the shin's own swing below is measured against where the ankle ENDED UP, not
    // where it started.
    vec3 from, to;
    glm_vec3_normalize_to(ab, from);
    glm_vec3_sub(knee_new, a, to);
    if (glm_vec3_norm(to) < IK_DIR_EPS)
        return;
    glm_vec3_normalize(to);

    versor q_hip;
    glm_quat_from_vecs(from, to, q_hip);

    vec3 ankle_rel, ankle_carried;
    glm_vec3_sub(c, a, ankle_rel);
    glm_quat_rotatev(q_hip, ankle_rel, ankle_rel);
    glm_vec3_add(a, ankle_rel, ankle_carried);

    // Every exit in this function leaves the chain at its animated pose, which is what
    // the header promises. This guard used to sit AFTER the three writes below, so the
    // one exit that could fire late left a thigh swung and a shin not -- a half-solved
    // chain, which is a state the contract says cannot occur.
    vec3 shin_from, shin_to;
    glm_vec3_sub(ankle_carried, knee_new, shin_from);
    glm_vec3_sub(ankle_new, knee_new, shin_to);
    if (glm_vec3_norm(shin_from) < IK_DIR_EPS || glm_vec3_norm(shin_to) < IK_DIR_EPS)
        return;
    glm_vec3_normalize(shin_from);
    glm_vec3_normalize(shin_to);

    rotate_global(g[f->hip_index], q_hip, a);
    rotate_global(g[f->knee_index], q_hip, knee_new);
    rotate_global(g[f->ankle_index], q_hip, ankle_carried);

    versor q_knee;
    glm_quat_from_vecs(shin_from, shin_to, q_knee);
    rotate_global(g[f->knee_index], q_knee, knee_new);
    rotate_global(g[f->ankle_index], q_knee, ankle_new);
}

// How much of this foot's plant actually applies: its requested weight, faded out as
// the clip lifts the foot clear of its target.
//
// ONE answer, used by both the pelvis drop and the leg solve. They disagreed once --
// the drop read the raw weight while the solve read the faded one -- so a foot the
// solve had released still dragged the hips down, the whole body bobbed, and every
// ankle moved with it. At frame rate that reads as legs scissoring in and out.
//
// Measured against `globals`, which at this point in the frame still hold the clip's
// own pose. A caller cannot answer this: by the time it runs they hold the last solve.
static float ik_effective_weight(const IkSystem* system, const IkFoot* f, mat4* globals) {
    if (f->weight <= 0.0f)
        return 0.0f;
    if (system->params.plant_fraction <= 0.0f)
        return f->weight;

    vec3 hip, knee, ankle, thigh, shin;
    glm_vec3_copy(globals[f->hip_index][3], hip);
    glm_vec3_copy(globals[f->knee_index][3], knee);
    glm_vec3_copy(globals[f->ankle_index][3], ankle);
    glm_vec3_sub(knee, hip, thigh);
    glm_vec3_sub(ankle, knee, shin);

    const float range =
        system->params.plant_fraction * (glm_vec3_norm(thigh) + glm_vec3_norm(shin));
    if (range <= 0.0f)
        return f->weight;

    const float lift = ankle[1] - f->applied_target[1];
    if (lift <= 0.0f)
        return f->weight;

    const float fall = 1.0f - lift / range;
    return fall > 0.0f ? f->weight * fall : 0.0f;
}

// Whether the CLIP has this foot on the ground: the toe low enough, and not travelling
// vertically. A statement about the animation rather than about the solve, and answered
// here for the same reason the release is -- these globals are still the clip's own pose.
//
// It is NOT the release test wearing a different name, and the two are deliberately
// separate. The release asks how much IK to apply and fades; this asks a yes or no
// about where the animation has put the foot, and a lock needs the second. Folding them
// would make a partly-released foot partly in contact, which is not a state the world
// has.
// It is judged at the ANKLE and held at the TOE, and that split was measured rather than
// chosen. Holden detects at the toe, on clips authored for the rig that plays them; on a
// clip retargeted onto a rig of other proportions the toe JOINT passes back through its
// own bind clearance in mid-swing -- traced at 0.0390 against a stance value of 0.0137 --
// and since that pass is the bottom of an arc it is momentarily slow as well, so neither
// a height threshold nor a speed threshold rejects it and the label breaks each stance
// into two. The ankle at the same instants reads 0.12 airborne against 0.085 planted,
// which separates cleanly. The toe stays the thing a lock HOLDS, because that is about
// where the contact is and not about how it is found.
static bool ik_contact_label(const IkSystem* system, const IkFoot* f, mat4* globals,
                             float delta_time) {
    vec3 hip, knee, ankle, thigh, shin;
    glm_vec3_copy(globals[f->hip_index][3], hip);
    glm_vec3_copy(globals[f->knee_index][3], knee);
    glm_vec3_copy(globals[f->ankle_index][3], ankle);
    glm_vec3_sub(knee, hip, thigh);
    glm_vec3_sub(ankle, knee, shin);
    const float leg = glm_vec3_norm(thigh) + glm_vec3_norm(shin);
    if (leg <= 0.0f)
        return false;

    // Against the target the caller set rather than against a plane, so a foot on a step
    // is judged by its own tread and not by the one the other foot is on. The target is
    // already the ankle's grounded height -- ik_foot_set_ground folded sole_offset in --
    // so this difference is the lift and needs no further correction.
    if (ankle[1] - f->target[1] > system->params.contact_height * leg)
        return false;

    // Speed needs two solves to exist. Before the second, height alone decides, which
    // is the right default: the alternative calls every foot airborne on the frame a
    // rig is created, and a lock that begins one frame late is visible.
    if (f->has_clip_prev && delta_time > 0.0f) {
        const float rise = fabsf(ankle[1] - f->clip_ankle_prev[1]) / delta_time;
        if (rise > system->params.contact_speed * leg)
            return false;
    }
    return true;
}

void ik_solve(IkSystem* system, mat4* global_transforms, float delta_time) {
    if (!system || !system->enabled || system->foot_count == 0 || !global_transforms)
        return;

    if (delta_time < 0.0f)
        delta_time = 0.0f;
    float ease = system->params.blend_rate * delta_time;
    if (ease > 1.0f)
        ease = 1.0f;

    // Ease, or snap. A target that jumped farther than teleport_distance did not move
    // -- the thing standing on it did -- and easing across that draws a foot through
    // the world instead of putting it down somewhere else.
    for (size_t i = 0; i < system->foot_count; i++) {
        IkFoot* f = &system->feet[i];
        const bool snap =
            system->needs_reset || !f->has_applied ||
            glm_vec3_distance(f->target, f->applied_target) > system->params.teleport_distance;
        if (snap) {
            glm_vec3_copy(f->target, f->applied_target);
            f->has_applied = true;
        } else {
            glm_vec3_lerp(f->applied_target, f->target, ease, f->applied_target);
        }

        // Settled HERE, once, and read unchanged by both consumers below. It has to be
        // here rather than in either of them: the pelvis drop mutates every ankle in
        // its subtree, so a second reading taken after it sees a smaller lift and
        // returns a larger weight -- the hips get lowered from one number and the legs
        // solved from another. This loop is the last point at which the globals are
        // still the clip's own pose, which is the only moment the question has a true
        // answer (see the header).
        f->applied_weight = ik_effective_weight(system, f, global_transforms);

        // The contact label, from the same globals and in the same window, then the
        // pose it will be differenced against next frame. Recorded even for a foot at
        // weight 0: whether the clip has a foot down is not conditional on whether IK
        // is being applied to it, and a label that went blank while a character was
        // airborne would have to be re-established on landing, one frame late.
        f->in_contact = ik_contact_label(system, f, global_transforms, delta_time);
        glm_vec3_copy(global_transforms[f->ankle_index][3], f->clip_ankle_prev);
        f->has_clip_prev = true;
    }

    // The pelvis drop, before any chain is solved, so every chain solves against where
    // the hips ENDED UP. A pure translation of the subtree: for a translation the
    // matrix product is just the offset added to the translation column, which is
    // exact, costs no multiply, and -- unlike a re-accumulation -- composes with
    // corrections the spring pass has already written.
    if (system->pelvis_index >= 0 && system->params.max_pelvis_drop > 0.0f) {
        float worst = 0.0f;
        // The leg length of the foot that asked for `worst`, so the cap below is a
        // fraction of the DEEPEST foot's leg rather than of each foot's own. The two
        // forms differ only when the legs differ in LENGTH -- with one cap for every
        // foot, max(min(d, cap)) and min(max(d), cap) are identically equal. No rig in
        // this tree is asymmetric, so nothing here can tell them apart.
        float worst_leg = 0.0f;
        for (size_t i = 0; i < system->foot_count; i++) {
            const IkFoot* f = &system->feet[i];
            // The same value the solve reads, settled before this loop moved anything.
            // Reading the raw weight here let a foot the solve had released still pull
            // the hips down, which bobbed the whole body every frame and read as legs
            // scissoring in and out.
            const float weight = f->applied_weight;
            if (weight <= 0.0f)
                continue; // released, or unweighted: it must not drag the hips down
            vec3 a, b, c, ab, bc;
            glm_vec3_copy(global_transforms[f->hip_index][3], a);
            glm_vec3_copy(global_transforms[f->knee_index][3], b);
            glm_vec3_copy(global_transforms[f->ankle_index][3], c);
            glm_vec3_sub(b, a, ab);
            glm_vec3_sub(c, b, bc);
            const float reach = glm_vec3_norm(ab) + glm_vec3_norm(bc);
            const float deficit =
                (glm_vec3_distance((float*)f->applied_target, a) - reach) * weight;
            if (deficit > worst) {
                worst = deficit;
                worst_leg = reach;
            }
        }
        if (worst > 0.0f) {
            const float cap = system->params.max_pelvis_drop * worst_leg;
            const float drop = worst < cap ? worst : cap;
            for (size_t i = 0; i < system->skeleton->bone_count && i < MAX_BONES; i++) {
                if (system->in_pelvis_subtree[i])
                    global_transforms[i][3][1] -= drop;
            }
        }
    }

    for (size_t i = 0; i < system->foot_count; i++) {
        const IkFoot* f = &system->feet[i];
        if (f->weight <= 0.0f)
            continue; // skipped entirely, so weight 0 is bit-identical to no IK

        const float weight = f->applied_weight;
        if (weight <= 0.0f)
            continue; // the clip has lifted this foot clear: leave the stride alone

        // Blend the TARGET rather than the matrices: a lerp of two rotations is
        // neither a rotation nor orthogonal, and would shear the limb on the way.
        vec3 ankle, effective;
        glm_vec3_copy(global_transforms[f->ankle_index][3], ankle);
        glm_vec3_lerp(ankle, (float*)f->applied_target, weight, effective);
        solve_two_bone(global_transforms, f, effective);
    }

    system->needs_reset = false;
}
