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
    // Half what it was, and set from the content rather than from taste. The technique's
    // author is explicit that dragging the hips down to reach produces "T-Rex" posturing
    // and that a little sliding is better than breaking the source animation, so the
    // question is how small this can be -- and the answer is whatever the deepest thing
    // in the world asks for. That is the demo staircase, whose 0.5 m riser leaves the
    // lower foot 0.25 m past reach: 0.305 of this rig's leg. 0.31 covers it and nothing
    // beyond it, where the 0.6098 that stood until spec 12.9 was double.
    //
    // Nothing had ever exercised the old cap -- spec 12.5 measured that: every fixture
    // asked for less than it, so min(deficit, cap) returned the deficit and a wrong cap
    // passed either way. ik-drop is what tests it now, and it tests the REFUSAL with it:
    // past the cap the foot is left short rather than the hips dragged further.
    p.max_pelvis_drop = 0.31f;
    p.teleport_distance = 0.5f;
    // Long enough to be smooth at 60 Hz and short enough that a foot is not still
    // catching up when the next contact arrives -- a stance on this walk is 0.7 s.
    p.transition_time = 0.15f;
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
    // A fifth of a leg of displacement is still close enough to the animation to pin to;
    // past half a leg the clip has walked away from the point and straining the chain to
    // hold it is worse than letting go. The gap between them is the hysteresis, and it is
    // wide because the cost of the two errors is not symmetric: an early unlock is a
    // little sliding, a flapping lock is a visible stutter.
    p.lock_distance = 0.15f;
    p.unlock_distance = 0.45f;
    return p;
}

void ik_set_world(IkSystem* system, mat4 model_to_world) {
    if (!system || !model_to_world) {
        log_error("ik_set_world: a null argument");
        return;
    }
    glm_mat4_copy(model_to_world, system->model_to_world);
    glm_mat4_inv(system->model_to_world, system->world_to_model);
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
    glm_mat4_identity(system->model_to_world);
    glm_mat4_identity(system->world_to_model);
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

    // The ankle's PROPER descendants: the subtree less the ankle itself, which the
    // solve writes explicitly. Resolved here because the chain cannot change afterwards.
    skeleton_mark_subtree(skeleton, ankle, foot->below_ankle);
    foot->below_ankle[ankle] = 0;

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

    // No bind-pose derivation here, unlike ik_add_foot's sole_offset. A toe's own
    // clearance above its sole would be the obvious thing to precompute and is not
    // wanted by anything: the lock captures the LIVE ankle-to-toe vector at the moment
    // it pins, so the foot's orientation at that instant is carried rather than the
    // bind's -- and it was the bind estimate being wrong on a retargeted clip that sent
    // contact detection to the ankle in the first place.
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
    // the jump the ankle appears to move the whole distance in one frame, which reads as
    // a foot travelling at an enormous rate and suppresses the label for exactly one
    // frame after every load. Dropping the history says the truth instead, which is
    // that there is no previous pose to compare with. And a held contact is worse than
    // stale -- it is a world point the character is no longer anywhere near, which the
    // unlock distance would release a frame later after one frame of a leg reaching for
    // the place it used to stand.
    for (size_t i = 0; i < system->foot_count; i++) {
        system->feet[i].has_clip_prev = false;
        system->feet[i].has_want_prev = false;
        system->feet[i].locked = false;
        system->feet[i].contact_pending = false;
    }
}

// The in-place form of skeleton_rotate_global, which is what every site here wants;
// naming it keeps the subscript off both sides of each call.
static void rotate_global(mat4 m, versor q, const vec3 head) {
    skeleton_rotate_global(m, m, q, head);
}

// The same rotation, about a PIVOT, applied to every bone the mask names. What the three
// chain writes do is this with the head stated directly; a descendant's head is not known
// in advance, so it comes out of the rotation instead.
//
// This is not re-accumulation and cannot be: ik_solve deliberately takes no
// local_transforms (the header says why). A rigid rotation about a point needs only the
// globals, which is exactly what a bone below the ankle undergoes.
static void rotate_below(mat4* g, const uint8_t* mask, size_t count, versor q, const vec3 pivot) {
    mat4 r;
    glm_quat_mat4(q, r);
    for (size_t i = 0; i < count && i < MAX_BONES; i++) {
        if (!mask[i])
            continue;
        vec3 rel;
        glm_vec3_sub(g[i][3], (float*)pivot, rel);
        glm_mat4_mulv3(r, rel, 0.0f, rel);
        mat4 turned;
        glm_mat4_mul(r, g[i], turned);
        glm_mat4_copy(turned, g[i]);
        glm_vec3_add((float*)pivot, rel, g[i][3]);
    }
}

// Rotate hip and knee so the ankle lands on `target`. Model space throughout, and only
// this chain's three bones are written. A degenerate segment leaves the chain at its
// animated pose, the way a spring bone follows rigidly below its own epsilon.
static void solve_two_bone(mat4* g, const IkFoot* f, const vec3 target, size_t bone_count) {
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
    //
    // Spec 12.9 REFUSED the exponential soft-clamp its method prescribes here, on that
    // same arithmetic re-measured: a soft band has to start BELOW full extension to
    // smooth anything, and on this rig's 0.42 + 0.40 leg a band of 1 per cent costs 16.2
    // degrees of permanent bend, 2 per cent costs 23.0 and 5 per cent costs 36.4. What
    // the softening buys is continuity in the ankle's VELOCITY as a target leaves reach
    // -- the position is already continuous, since min(want, hi) is. A visible bend in
    // every near-straight leg to smooth a kink in an unreachable one is the wrong trade,
    // and it is the wrong trade specifically because this rig binds straight. On a rig
    // that binds with a bent knee the same band costs a fraction of this, and the
    // refusal should be re-measured rather than inherited.
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
    // The thigh swings the whole limb, and the foot is part of the limb. About the HIP,
    // which is what the three writes above turn around.
    rotate_below(g, f->below_ankle, bone_count, q_hip, a);

    versor q_knee;
    glm_quat_from_vecs(shin_from, shin_to, q_knee);
    rotate_global(g[f->knee_index], q_knee, knee_new);
    rotate_global(g[f->ankle_index], q_knee, ankle_new);
    rotate_below(g, f->below_ankle, bone_count, q_knee, knee_new);
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
static float ik_effective_weight(const IkSystem* system, const IkFoot* f, float ankle_y,
                                 float leg) {
    if (f->weight <= 0.0f)
        return 0.0f;
    if (system->params.plant_fraction <= 0.0f)
        return f->weight;
    // A locked foot does not fade. The release exists to stop a foot the clip has lifted
    // being dragged back down, and it answers that from HEIGHT ALONE because until this
    // spec there was nothing better to ask. The contact label is the better thing, and it
    // is the STRICTER of the two -- contact_height is 0.05 of a leg against
    // plant_fraction's 0.10 -- so a locked foot is one the release would only have
    // part-faded anyway. Letting it fade costs the whole feature: measured, the fade ran
    // the first five ticks of every stance at 0.63 to 1.00 weight, the foot could not hold
    // the point it had just pinned, and the slide came out WORSE than planting's.
    if (f->locked)
        return f->weight;

    const float range = system->params.plant_fraction * leg;
    if (range <= 0.0f)
        return f->weight;

    const float lift = ankle_y - f->applied_target[1];
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
static bool ik_contact_label(const IkSystem* system, const IkFoot* f, mat4* globals, float leg,
                             float delta_time) {
    if (leg <= 0.0f)
        return false;
    vec3 ankle;
    glm_vec3_copy(globals[f->ankle_index][3], ankle);

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

// The lock, and the ankle target it asks for. Writes `want` in every case, so the caller
// has one target to ease toward whether or not anything is pinned.
//
// The solve underneath is untouched by this and that is the design: it receives a point
// and neither knows nor cares whether the point came from a live ray or from a contact
// frozen four frames ago. Locking is a layer in FRONT of solve_two_bone, not a change
// to it.
// Returns true on the frame a contact is PINNED, which the caller takes verbatim instead
// of easing into. Easing into a lock is the one case where the ease is wrong: by then
// applied_target has spent the whole swing chasing a fast-moving foot and is a long way
// behind it, so the lock inherits that lag and spends the stance paying it off -- measured
// at 0.13 m of world travel before this, against 0.005 m once converged. The label has
// already established the foot is at that point; the ease would be smoothing a
// discontinuity that is not there.
static bool ik_lock_update(IkSystem* system, IkFoot* f, mat4* globals, float leg, vec3 want) {
    glm_vec3_copy(f->target, want);
    // No weight, no contact. weight is the caller saying it is planting this foot at all,
    // and a foot it is not planting has no target worth pinning -- during a jump the
    // target is the ankle's own position at weight 0, which would pin a contact in
    // mid-air. It also keeps a lock out of the frames before a caller has established
    // where the rig stands, since those are the frames it holds the weight at zero.
    if (system->params.lock_distance <= 0.0f || f->weight <= 0.0f) {
        const bool was = f->locked;
        f->locked = false;
        f->contact_pending = false;
        return was;
    }
    bool pinned = false, released = false;

    vec3 ankle, toe, foot_vec;
    glm_vec3_copy(globals[f->ankle_index][3], ankle);
    glm_vec3_copy(globals[f->toe_index][3], toe);
    // This frame's ankle-to-toe vector, carrying the foot's current orientation. Read
    // live rather than from the bind pose, so a foot the clip has pitched onto its toe
    // converts through the pitch it actually has.
    glm_vec3_sub(toe, ankle, foot_vec);

    if (f->locked && !f->contact_pending) {
        vec3 toe_world;
        glm_mat4_mulv3(system->model_to_world, toe, 1.0f, toe_world);
        // Either condition releases: the clip picked the foot up, or it has carried the
        // foot so far from the pinned point that holding it would strain the chain. The
        // second is what makes an unreachable lock a non-event rather than a stretch.
        if (!f->in_contact ||
            glm_vec3_distance(toe_world, f->contact_world) > system->params.unlock_distance * leg) {
            f->locked = false;
            released = true;
        }
    } else if (f->in_contact) {
        // How far the solve is already displacing this foot from where the animation put
        // it. Pinning while that is large welds the foot at a place the clip disagrees
        // with, and the correction gets paid on every frame of the stance instead of
        // once at the transition.
        if (glm_vec3_distance(ankle, f->target) <= system->params.lock_distance * leg) {
            // Decided here, captured after the solve. This frame keeps the caller's
            // target, so the pin itself moves nothing and the point taken at the end of
            // it is where the foot genuinely is -- no offset to blend, and the height
            // clamp the method prescribes is unnecessary because the solve has already
            // put the ankle on the ground it was handed.
            f->locked = true;
            f->contact_pending = true;
            pinned = true;
        }
    }

    if (f->locked && !f->contact_pending) {
        // The pinned toe, back in model space, less this frame's foot vector: the ankle
        // that puts the toe where it was left. The heel is free to lift, which is what
        // preserves the clip's own toe-off.
        vec3 contact_model;
        glm_mat4_mulv3(system->world_to_model, f->contact_world, 1.0f, contact_model);
        glm_vec3_sub(contact_model, foot_vec, want);
    }
    return pinned || released;
}

// The captured discontinuity, decayed. A cubic Hermite from (offset_pos, offset_vel) at
// the transition to (0, 0) at transition_time: it leaves at the offset the foot actually
// had, at the speed it actually had, and arrives flat. Carrying the VELOCITY is the whole
// difference from a lerp -- a foot that reaches a lock still moving keeps moving for a
// moment and then settles, instead of being stopped dead and dragged.
//
// The other half of what this buys is silent: once u reaches 1 the offset is exactly
// zero, so the applied target IS the requested one. An exponential ease is never
// finished, and against a moving target that unfinished remainder is a standing error.
static void ik_inertial_offset(const IkFoot* f, float span, vec3 out) {
    const float u = span > 0.0f ? f->since_transition / span : 1.0f;
    if (u >= 1.0f) {
        glm_vec3_zero(out);
        return;
    }
    const float u2 = u * u, u3 = u2 * u;
    vec3 p, v;
    glm_vec3_scale((float*)f->offset_pos, 2.0f * u3 - 3.0f * u2 + 1.0f, p);
    glm_vec3_scale((float*)f->offset_vel, span * (u3 - 2.0f * u2 + u), v);
    glm_vec3_add(p, v, out);
}

void ik_solve(IkSystem* system, mat4* global_transforms, float delta_time) {
    if (!system || !system->enabled || system->foot_count == 0 || !global_transforms)
        return;

    if (delta_time < 0.0f)
        delta_time = 0.0f;
    const float span = system->params.transition_time;

    // Blend, or snap. A target that jumped farther than teleport_distance did not move
    // -- the thing standing on it did -- and blending across that draws a foot through
    // the world instead of putting it down somewhere else.
    for (size_t i = 0; i < system->foot_count; i++) {
        IkFoot* f = &system->feet[i];

        // The label and then the lock, both BEFORE the ease, because both read the
        // globals as the clip left them and the ease is what turns the answer into a
        // target. `want` is the caller's target, or the pinned contact's, and from here
        // down nothing else needs to know which.
        vec3 hip, knee, ankle, thigh, shin;
        glm_vec3_copy(global_transforms[f->hip_index][3], hip);
        glm_vec3_copy(global_transforms[f->knee_index][3], knee);
        glm_vec3_copy(global_transforms[f->ankle_index][3], ankle);
        glm_vec3_sub(knee, hip, thigh);
        glm_vec3_sub(ankle, knee, shin);
        const float leg = glm_vec3_norm(thigh) + glm_vec3_norm(shin);

        f->in_contact = ik_contact_label(system, f, global_transforms, leg, delta_time);
        // Zeroed because cppcheck cannot see through an out-parameter and reads it as
        // used before set; ik_lock_update writes it on its first line in every path.
        vec3 want = {0.0f, 0.0f, 0.0f};
        const bool changed = ik_lock_update(system, f, global_transforms, leg, want);

        // The requested target's own velocity, taken before anything is blended onto it,
        // so a transition can capture the MISMATCH in velocity and not just in position.
        vec3 want_vel = {0.0f, 0.0f, 0.0f};
        if (f->has_want_prev && delta_time > 0.0f) {
            glm_vec3_sub(want, f->want_prev, want_vel);
            glm_vec3_scale(want_vel, 1.0f / delta_time, want_vel);
        }

        const bool snap =
            system->needs_reset || !f->has_applied ||
            glm_vec3_distance(want, f->applied_target) > system->params.teleport_distance;
        if (snap) {
            // A hard snap carries NO offset. This is the case where the foot is meant to
            // appear somewhere else rather than travel there, so smoothing it is the one
            // thing that would be wrong.
            glm_vec3_zero(f->offset_pos);
            glm_vec3_zero(f->offset_vel);
            f->since_transition = span;
            f->has_applied = true;
        } else if (changed && span > 0.0f) {
            // A lock or a release. Capture where the output is against where the input
            // has just jumped to, in position and in velocity, and let it decay.
            vec3 out_vel = {0.0f, 0.0f, 0.0f};
            if (delta_time > 0.0f) {
                glm_vec3_sub(f->applied_target, f->applied_prev, out_vel);
                glm_vec3_scale(out_vel, 1.0f / delta_time, out_vel);
            }
            glm_vec3_sub(f->applied_target, want, f->offset_pos);
            glm_vec3_sub(out_vel, want_vel, f->offset_vel);
            // The velocity term may not carry the output FURTHER from the target than the
            // position offset already is. A lock is captured at a position offset of very
            // nearly zero -- the pinned point is the grounded ankle, which is what the
            // caller's target already was -- while the velocity mismatch is the whole of
            // the swing speed, so uncapped the blend takes the foot out and brings it
            // back: measured at 0.030 m of excursion, which is the slide the lock exists
            // to remove, reintroduced by the thing meant to smooth it. Capped, velocity
            // continuity still has its effect wherever there is a real offset to blend.
            for (int k = 0; k < 3; k++) {
                const float cap = fabsf(f->offset_pos[k]) / span;
                f->offset_vel[k] = glm_clamp(f->offset_vel[k], -cap, cap);
            }
            f->since_transition = 0.0f;
        } else {
            f->since_transition += delta_time;
        }

        glm_vec3_copy(f->applied_target, f->applied_prev);
        // Zeroed for cppcheck, which reads an out-parameter as used before set.
        vec3 offset = {0.0f, 0.0f, 0.0f};
        ik_inertial_offset(f, span, offset);
        glm_vec3_add(want, offset, f->applied_target);
        glm_vec3_copy(want, f->want_prev);
        f->has_want_prev = true;

        // Settled HERE, once, and read unchanged by both consumers below. It has to be
        // here rather than in either of them: the pelvis drop mutates every ankle in
        // its subtree, so a second reading taken after it sees a smaller lift and
        // returns a larger weight -- the hips get lowered from one number and the legs
        // solved from another. This loop is the last point at which the globals are
        // still the clip's own pose, which is the only moment the question has a true
        // answer (see the header).
        f->applied_weight = ik_effective_weight(system, f, ankle[1], leg);

        // The pose next frame's contact speed is differenced against. Recorded even for
        // a foot at weight 0: whether the clip has a foot down is not conditional on
        // whether IK is being applied to it, and a history that went blank while a
        // character was airborne would have to be re-established on landing, one frame
        // late.
        glm_vec3_copy(ankle, f->clip_ankle_prev);
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
        solve_two_bone(global_transforms, f, effective, system->skeleton->bone_count);
    }

    // The contacts decided this frame, captured now that the feet are where they will be
    // drawn. A point taken before the solve is a point the foot is not yet at, and the
    // lock then spends its first ticks dragging the foot onto it -- which is travel
    // across the ground, and reads as the very thing the lock removes. Taken here the
    // pin moves nothing at all: the frame it happens on is the frame the caller's own
    // target was used.
    for (size_t i = 0; i < system->foot_count; i++) {
        IkFoot* f = &system->feet[i];
        if (!f->contact_pending)
            continue;
        glm_mat4_mulv3(system->model_to_world, global_transforms[f->toe_index][3], 1.0f,
                       f->contact_world);
        f->contact_pending = false;
    }

    system->needs_reset = false;
}
