#include "look_at.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"

// Below this the head is the clip's own, exactly: the solve does not run at all.
#define LOOK_AT_WEIGHT_EPS 1e-4f
// Elevation is held off straight up and straight down, where heading has no meaning and a
// yaw about the up axis turns nothing.
#define LOOK_AT_ELEVATION_MAX 1.4f

LookAtSystem* create_look_at_system(Skeleton* skeleton) {
    if (!skeleton) {
        log_error("create_look_at_system: no skeleton");
        return NULL;
    }
    LookAtSystem* s = calloc(1, sizeof(LookAtSystem));
    if (!s) {
        log_error("create_look_at_system: out of memory");
        return NULL;
    }
    s->skeleton = skeleton;
    s->parent = -1;
    glm_mat4_identity(s->model_to_world);
    glm_mat4_identity(s->world_to_model);
    s->max_yaw = glm_rad(70.0f);
    s->max_pitch = glm_rad(40.0f);
    s->give_up = glm_rad(110.0f);
    s->rate = 6.0f;
    s->blend_rate = 4.0f;
    return s;
}

void free_look_at_system(LookAtSystem* system) {
    free(system);
}

bool look_at_add_bone(LookAtSystem* system, const char* name, float share) {
    if (!system || !name) {
        log_error("look_at_add_bone: a null argument");
        return false;
    }
    if (system->bone_count >= LOOK_AT_MAX_BONES) {
        log_error("look_at_add_bone: the chain is full at %d bones; '%s' refused",
                  LOOK_AT_MAX_BONES, name);
        return false;
    }
    Skeleton* sk = system->skeleton;
    const int bone = get_bone_index_by_name(sk, name);
    if (bone < 0) {
        log_error("look_at_add_bone: skeleton '%s' has no bone '%s'", sk->name, name);
        return false;
    }
    if (system->bone_count > 0) {
        const LookAtBone* above = &system->bones[system->bone_count - 1];
        bool below = false;
        for (size_t k = 1; k < above->carried_count && !below; k++)
            below = above->carried[k] == bone;
        if (!below) {
            log_error("look_at_add_bone: '%s' does not descend from '%s'", name,
                      sk->bones[above->bone].name);
            return false;
        }
    } else {
        system->parent = sk->bones[bone].parent_index;
    }
    LookAtBone* b = &system->bones[system->bone_count++];
    b->bone = bone;
    b->share = share;
    uint8_t mask[MAX_BONES];
    skeleton_mark_subtree(sk, bone, mask);
    b->carried_count = 0;
    for (size_t i = 0; i < sk->bone_count; i++)
        if (mask[i])
            b->carried[b->carried_count++] = (uint8_t)i;
    return true;
}

void look_at_set_world(LookAtSystem* system, mat4 model_to_world) {
    if (!system || !model_to_world) {
        log_error("look_at_set_world: a null argument");
        return;
    }
    glm_mat4_copy(model_to_world, system->model_to_world);
    glm_mat4_inv(system->model_to_world, system->world_to_model);
}

// The rig's frame: forward, up, and the right they make, with the zero defaults applied.
typedef struct Frame {
    vec3 forward, up, right;
} Frame;

static void rig_frame(const LookAtSystem* s, Frame* f) {
    if (glm_vec3_norm2((float*)s->forward) > 0.0f)
        glm_vec3_normalize_to((float*)s->forward, f->forward);
    else
        glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, f->forward);
    if (glm_vec3_norm2((float*)s->up) > 0.0f)
        glm_vec3_normalize_to((float*)s->up, f->up);
    else
        glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, f->up);
    glm_vec3_cross(f->up, f->forward, f->right);
    glm_vec3_normalize(f->right);
}

static float heading_of(const Frame* f, const vec3 v) {
    return atan2f(glm_vec3_dot((float*)v, (float*)f->right),
                  glm_vec3_dot((float*)v, (float*)f->forward));
}

static float elevation_of(const Frame* f, const vec3 v) {
    return asinf(glm_clamp(glm_vec3_dot((float*)v, (float*)f->up), -1.0f, 1.0f));
}

// Which way a bone's copy of the rig's forward points now: its bind frame carries the model
// forward into the bone, and its global carries that back out as posed.
static void bone_forward(const LookAtSystem* s, mat4* g, int bone, const Frame* f, vec3 out) {
    if (bone < 0) {
        glm_vec3_copy((float*)f->forward, out);
        return;
    }
    mat4 skin;
    glm_mat4_mul(g[bone], s->skeleton->bones[bone].inverse_bind_pose, skin);
    glm_mat4_mulv3(skin, (float*)f->forward, 0.0f, out);
    glm_vec3_normalize(out);
}

static void eye_now(const LookAtSystem* s, mat4* g, vec3 out) {
    const int last = s->bones[s->bone_count - 1].bone;
    if (glm_vec3_norm2((float*)s->eye) == 0.0f) {
        glm_vec3_copy(g[last][3], out);
        return;
    }
    mat4 skin;
    glm_mat4_mul(g[last], s->skeleton->bones[last].inverse_bind_pose, skin);
    glm_mat4_mulv3(skin, (float*)s->eye, 1.0f, out);
}

// Rotate a bone and everything below it about the bone's own head: a yaw of `yaw` about the
// rig's up, then a pitch of `pitch` about the level axis across the heading the yaw left
// the aim at.
static void turn_bone(const LookAtSystem* s, mat4* g, const LookAtBone* b, const Frame* f,
                      float heading, float yaw, float pitch) {
    versor qy, qp, q;
    glm_quatv(qy, yaw, (float*)f->up);
    const float h = heading + yaw;
    vec3 level, axis;
    glm_vec3_scale((float*)f->forward, cosf(h), level);
    glm_vec3_muladds((float*)f->right, sinf(h), level);
    glm_vec3_cross(level, (float*)f->up, axis);
    glm_quatv(qp, pitch, axis);
    glm_quat_mul(qp, qy, q);
    vec3 pivot;
    glm_vec3_copy(g[b->bone][3], pivot);
    skeleton_rotate_bones(g, b->carried, b->carried_count, q, pivot);
}

void look_at_solve(LookAtSystem* s, mat4* g, float dt) {
    if (!s || !g || s->bone_count == 0)
        return;
    if (!s->has_target && s->weight == 0.0f) {
        s->tracking = false;
        return;
    }
    Frame f;
    rig_frame(s, &f);
    const int last = s->bones[s->bone_count - 1].bone;

    vec3 eye = {0.0f, 0.0f, 0.0f}, to = {0.0f, 0.0f, 0.0f};
    eye_now(s, g, eye);
    bool want = s->has_target;
    if (want) {
        vec3 t;
        glm_mat4_mulv3(s->world_to_model, s->target, 1.0f, t);
        glm_vec3_sub(t, eye, to);
        const float d = glm_vec3_norm(to);
        if (d < 1e-4f)
            want = false;
        else
            glm_vec3_scale(to, 1.0f / d, to);
    }

    vec3 ref = {0.0f, 0.0f, 1.0f};
    bone_forward(s, g, s->parent, &f, ref);
    const float ref_heading = heading_of(&f, ref), ref_elevation = elevation_of(&f, ref);
    if (want) {
        // Let go past give_up and take it back only once it is within reach again, so a
        // target on the boundary does not flick the head back and forth.
        const float off = fabsf(wrap_pi(heading_of(&f, to) - ref_heading));
        if (off > s->give_up)
            s->tracking = false;
        else if (off <= s->max_yaw)
            s->tracking = true;
        want = s->tracking;
    } else {
        s->tracking = false;
    }

    const bool fresh = s->weight == 0.0f;
    const float ease = dt > 0.0f ? 1.0f - expf(-s->blend_rate * dt) : 0.0f;
    s->weight += ((want ? 1.0f : 0.0f) - s->weight) * ease;
    if (!want && s->weight < LOOK_AT_WEIGHT_EPS)
        s->weight = 0.0f;
    if (s->weight == 0.0f)
        return;

    vec3 aim = {0.0f, 0.0f, 1.0f};
    bone_forward(s, g, last, &f, aim);
    if (fresh) {
        // Taken up from wherever the clip has the head, so nothing snaps.
        glm_mat4_mulv3(s->model_to_world, aim, 0.0f, s->gaze);
        glm_vec3_normalize(s->gaze);
    }
    if (want) {
        vec3 to_world;
        glm_mat4_mulv3(s->model_to_world, to, 0.0f, to_world);
        glm_vec3_normalize(to_world);
        const float k = dt > 0.0f ? 1.0f - expf(-s->rate * dt) : 0.0f;
        vec3 next;
        glm_vec3_lerp(s->gaze, to_world, k, next);
        if (glm_vec3_norm2(next) > 1e-8f) {
            glm_vec3_normalize(next);
            glm_vec3_copy(next, s->gaze);
        }
    }
    vec3 gaze;
    glm_mat4_mulv3(s->world_to_model, s->gaze, 0.0f, gaze);
    glm_vec3_normalize(gaze);

    // Where the head is to point, clamped against the parent, then as much of the way there
    // from the clip as the weight says.
    const float yaw =
        glm_clamp(wrap_pi(heading_of(&f, gaze) - ref_heading), -s->max_yaw, s->max_yaw);
    const float pitch =
        glm_clamp(elevation_of(&f, gaze) - ref_elevation, -s->max_pitch, s->max_pitch);
    const float want_heading = ref_heading + yaw;
    const float want_elevation =
        glm_clamp(ref_elevation + pitch, -LOOK_AT_ELEVATION_MAX, LOOK_AT_ELEVATION_MAX);
    const float aim_heading = heading_of(&f, aim), aim_elevation = elevation_of(&f, aim);
    const float goal_heading = aim_heading + s->weight * wrap_pi(want_heading - aim_heading);
    const float goal_elevation = aim_elevation + s->weight * (want_elevation - aim_elevation);

    // Down the chain, each bone taking its share of what is LEFT, read from the aim as the
    // bones above have already turned it: so the last bone takes all of the remainder and
    // the aim lands on the goal exactly, whatever the pitch axes of the earlier turns did.
    float remaining = 0.0f;
    for (int i = 0; i < s->bone_count; i++)
        remaining += fmaxf(s->bones[i].share, 0.0f);
    if (remaining <= 0.0f)
        return;
    for (int i = 0; i < s->bone_count; i++) {
        const LookAtBone* b = &s->bones[i];
        const float share = fmaxf(b->share, 0.0f);
        const float part = share / remaining;
        remaining -= share;
        if (part <= 0.0f)
            continue;
        vec3 now = {0.0f, 0.0f, 1.0f};
        bone_forward(s, g, last, &f, now);
        const float h = heading_of(&f, now), e = elevation_of(&f, now);
        turn_bone(s, g, b, &f, h, part * wrap_pi(goal_heading - h), part * (goal_elevation - e));
        if (remaining <= 0.0f)
            break;
    }
}
