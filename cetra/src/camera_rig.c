#include "camera_rig.h"

#include <stdlib.h>

#include "camera.h"
#include "ext/log.h"

// Below this the eye and the aim point coincide and there is no aim POINT to
// give, only an aim direction. See the header: the target goes one unit out.
#define RIG_FIRST_PERSON_EPS 1e-4f

CameraRig* create_camera_rig(void) {
    CameraRig* rig = calloc(1, sizeof(CameraRig));
    if (!rig) {
        log_error("create_camera_rig: out of memory");
        return NULL;
    }
    camera_rig_set_distance(rig, 5.0f);
    rig->pitch_min = -1.5f;
    rig->pitch_max = 1.5f;
    // Radians a second at full deflection. A comfortable turn for a stick or a
    // held key, which is slower than a hand on a mouse would want -- a key
    // cannot modulate its own rate, so the rate has to suit a whole turn rather
    // than a glance.
    rig->yaw_rate = 1.8f;
    rig->pitch_rate = 1.2f;
    rig->look_scale = 1.0f;
    rig->widen_rate = 4.0f;
    rig->tighten_rate = 1.2f;
    rig->probe_skin = 0.6f;
    rig->shake_scale = 1.0f;
    rig->shake_player_scale = 1.0f;
    rig->shake_freq = 22.0f;
    return rig;
}

void free_camera_rig(CameraRig* rig) {
    free(rig);
}

void camera_rig_set_distance(CameraRig* rig, float dist) {
    if (!rig) {
        log_error("camera_rig_set_distance: NULL rig");
        return;
    }
    // All three, so the response the next update runs lerps between two copies
    // of this and leaves it where the caller put it. Setting `dist` alone would
    // be overwritten on the next tick by ends nobody moved.
    rig->near_dist = rig->far_dist = rig->dist = dist;
}

// The aim direction. One derivation, where there were three.
void camera_rig_direction(const CameraRig* rig, vec3 out) {
    const float cp = cosf(rig->pitch);
    out[0] = sinf(rig->yaw) * cp;
    out[1] = sinf(rig->pitch);
    out[2] = cosf(rig->yaw) * cp;
}

// An exponential approach, frame-rate independent: the fraction covered in dt
// is 1 - e^(-rate*dt), so two half-length steps land where one long one does.
static float _rig_approach(float now, float want, float rate, float dt) {
    return now + (want - now) * (1.0f - expf(-rate * dt));
}

// The rail at `t`, as Catmull-Rom through the authored points. Hermite with
// tangents from the neighbours, which is the same curve.
static void _rig_rail_at(const CameraRig* rig, float t, vec3 out) {
    const int n = rig->rail_count;
    const float span = rig->rail_loop ? (float)n : (float)(n - 1);
    float u = glm_clamp(t, 0.0f, 1.0f) * span;
    int i = (int)u;
    if (i >= (int)span)
        i = (int)span - 1;
    const float s = u - (float)i;

    // Neighbours: a loop wraps, an open rail repeats its ends so the first and
    // last segments have a tangent at all.
    const int i0 = rig->rail_loop ? (i - 1 + n) % n : glm_max(i - 1, 0);
    const int i1 = rig->rail_loop ? i % n : i;
    const int i2 = rig->rail_loop ? (i + 1) % n : glm_min(i + 1, n - 1);
    const int i3 = rig->rail_loop ? (i + 2) % n : glm_min(i + 2, n - 1);

    for (int c = 0; c < 3; c++) {
        const float p0 = rig->rail[i0][c], p1 = rig->rail[i1][c];
        const float p2 = rig->rail[i2][c], p3 = rig->rail[i3][c];
        // Catmull-Rom's tangents, handed to Hermite's basis.
        vec4 hermite = {p1, p2, 0.5f * (p2 - p0), 0.5f * (p3 - p1)};
        out[c] = glm_smc(s, GLM_HERMITE_MAT, hermite);
    }
}

/*
 * The ONE writer of the aim under a change: the player's preference, then the
 * clamp, in that order.
 *
 * Every way of turning goes through here -- a rate over dt, an absolute aim, a
 * drag's pixels -- which is what makes "a pitch being CHANGED is clamped" one
 * rule rather than three sites that must agree. It is also what puts the look
 * preference on every path: before this existed it reached the rate path alone,
 * which is one of three ways a camera in this tree is turned.
 *
 * camera_rig_set_pose is the one entry that writes the aim WITHOUT coming
 * through here, and that is the exception the header states: a stated pose is
 * stated, and pulling it into a steering band would make an exact framing
 * impossible to ask for.
 */
static void _rig_turn(CameraRig* rig, float dyaw, float dpitch) {
    if (dyaw == 0.0f && dpitch == 0.0f)
        return;
    rig->yaw += dyaw * rig->look_scale;
    rig->pitch += (rig->invert_pitch ? -dpitch : dpitch) * rig->look_scale;
    rig->pitch = glm_clamp(rig->pitch, rig->pitch_min, rig->pitch_max);
    rig->pose_stated = false;
}

// Where the eye and the aim point go, from the rig's fields alone. PURE: it
// reads, and writes only through `out`. That is what lets the caller compare a
// derivation against a stated pose instead of enumerating everything that could
// have invalidated one.
static void _rig_derive(const CameraRig* rig, CameraRigPose* out) {
    vec3 dir = {0.0f, 0.0f, 0.0f};
    camera_rig_direction(rig, dir);

    vec3 look;
    glm_vec3_copy((float*)rig->anchor, look);
    look[1] += rig->look_lift;

    if (rig->rail_count >= 2) {
        // The rail states where the eye IS, so the arm, its response and the
        // probe have nothing to say about it; the aim is unchanged, which is
        // what makes a dolly past a subject one call and not a mode.
        _rig_rail_at(rig, rig->rail_t, out->eye);
        glm_vec3_copy(look, out->look);
        return;
    }

    /*
     * The arm is probed as an ARM -- from the aim point, along `dir`, for
     * exactly `dist` -- and the lift is added afterwards.
     *
     * Probing the lifted segment instead makes its length not equal the `want`
     * handed to the probe, so a probe answering a fraction of what it was told
     * is answering a fraction of the wrong thing: at a lift of 9 over an arm of
     * 18 the segment is 21.5, and every hit reads 19% nearer than it is. On the
     * ground the lift is 0 and the two agree, which is why nothing that walks
     * can see it.
     */
    float arm = rig->dist;
    if (rig->probe && arm > RIG_FIRST_PERSON_EPS) {
        vec3 want_eye;
        glm_vec3_copy(look, want_eye);
        glm_vec3_mulsubs(dir, arm, want_eye);
        const float clear = glm_clamp(rig->probe(rig->probe_user, look, want_eye, arm), 0.0f, 1.0f);
        if (clear * arm < arm)
            arm = glm_max(clear * arm - rig->probe_skin, rig->min_dist);
    }

    glm_vec3_copy(look, out->eye);
    out->eye[1] += rig->eye_lift;
    glm_vec3_mulsubs(dir, arm, out->eye);

    if (arm < RIG_FIRST_PERSON_EPS) {
        // First person has no aim POINT, only an aim direction. See the header.
        glm_vec3_add(out->eye, dir, out->look);
    } else {
        glm_vec3_copy(look, out->look);
    }
}

void camera_rig_update(CameraRig* rig, float dt, float yaw_in, float pitch_in) {
    if (!rig) {
        log_error("camera_rig_update: NULL rig");
        return;
    }

    _rig_turn(rig, -yaw_in * rig->yaw_rate * dt, pitch_in * rig->pitch_rate * dt);
    rig->shake_clock += dt;

    rig->wide = _rig_approach(rig->wide, glm_clamp(rig->want_wide, 0.0f, 1.0f),
                              rig->want_wide > rig->wide ? rig->widen_rate : rig->tighten_rate, dt);
    rig->dist = glm_lerp(rig->near_dist, rig->far_dist, rig->wide);
    rig->eye_lift = glm_lerp(rig->near_eye_lift, rig->far_eye_lift, rig->wide);
    if (rig->max_dist > 0.0f)
        rig->dist = glm_min(rig->dist, rig->max_dist);

    /*
     * A pose that was STATED is kept verbatim while the derivation still agrees
     * with it, and the test is the derivation's own OUTPUT rather than a list of
     * everything that could have changed.
     *
     * The list is what this was first: a flag cleared by five setters plus an
     * anchor comparison. It was already incomplete -- a write to look_lift, to
     * either end of the response, to max_dist or to the probe went silently
     * ignored while a pose was held -- and every field added later would have
     * had to remember to join it. Comparing the answer cannot fall behind the
     * fields.
     *
     * Bitwise equality is the right test and not a tolerance: the same code over
     * the same unchanged floats gives the same bits, and set_pose pins near_dist
     * to far_dist and both lifts to zero, so the response cannot perturb the
     * derivation of a held pose.
     */
    CameraRigPose derived;
    _rig_derive(rig, &derived);
    if (rig->pose_stated && glm_vec3_eqv(derived.eye, rig->stated_derive)) {
        rig->base = rig->stated;
    } else {
        rig->base = derived;
        rig->pose_stated = false;
    }

    /*
     * The modifiers ride on `base` and are written into `pose`, which is what
     * keeps them off their own output: writing a blend back into the thing being
     * blended FROM makes the second frame interpolate toward last frame's
     * interpolation, and the camera converges on a pose neither end asked for.
     */
    rig->pose = rig->base;

    if (rig->blend_left > 0.0f) {
        rig->blend_left = glm_max(rig->blend_left - dt, 0.0f);
        const float t = glm_smoothstep(0.0f, 1.0f, 1.0f - rig->blend_left / rig->blend_secs);
        glm_vec3_lerp(rig->blend_from.eye, rig->base.eye, t, rig->pose.eye);
        glm_vec3_lerp(rig->blend_from.look, rig->base.look, t, rig->pose.look);
    }

    if (rig->shake_left > 0.0f) {
        rig->shake_left = glm_max(rig->shake_left - dt, 0.0f);
        const float a = rig->shake_amp * rig->shake_scale * rig->shake_player_scale *
                        (rig->shake_left / rig->shake_secs) * (rig->shake_left / rig->shake_secs);
        const float w = rig->shake_clock * rig->shake_freq;
        // Ratios chosen so the common period is far longer than a shake lasts,
        // rather than short enough for the eye to find the repeat.
        rig->pose.eye[0] += a * sinf(w);
        rig->pose.eye[1] += a * sinf(w * 1.37f + 1.7f);
        rig->pose.eye[2] += a * sinf(w * 0.83f + 4.1f);
    }

    rig->posed = true;
}

void camera_rig_aim(CameraRig* rig, float yaw, float pitch) {
    if (!rig) {
        log_error("camera_rig_aim: NULL rig");
        return;
    }
    // Through the one writer, as a delta, so an absolute aim gets the same
    // preference and the same clamp a rate does.
    _rig_turn(rig, yaw - rig->yaw, pitch - rig->pitch);
}

void camera_rig_frame_sphere(CameraRig* rig, const vec3 centre, float radius, float fit) {
    if (!rig || !centre) {
        log_error("camera_rig_frame_sphere: NULL rig or centre");
        return;
    }
    if (radius <= 0.0f) {
        log_error("camera_rig_frame_sphere: a radius of %g frames nothing", (double)radius);
        return;
    }
    glm_vec3_copy((float*)centre, rig->anchor);
    camera_rig_set_distance(rig, radius * (fit > 0.0f ? fit : 2.5f));
}

void camera_rig_set_probe(CameraRig* rig, CameraRigProbeFn probe, void* user) {
    if (!rig) {
        log_error("camera_rig_set_probe: NULL rig");
        return;
    }
    rig->probe = probe;
    rig->probe_user = probe ? user : NULL;
}

bool camera_rig_set_rail(CameraRig* rig, const vec3* points, int count, bool loop) {
    if (!rig) {
        log_error("camera_rig_set_rail: NULL rig");
        return false;
    }
    if (!points || count <= 0) {
        rig->rail_count = 0;
        return true;
    }
    if (count < 2) {
        log_error("camera_rig_set_rail: %d point(s) is not a path", count);
        return false;
    }
    if (count > CAMERA_RIG_RAIL_MAX) {
        log_error("camera_rig_set_rail: %d points, max %d -- refused rather than truncated, "
                  "since a rail missing its end stops somewhere nobody chose",
                  count, CAMERA_RIG_RAIL_MAX);
        return false;
    }
    for (int i = 0; i < count; i++)
        glm_vec3_copy((float*)points[i], rig->rail[i]);
    rig->rail_count = count;
    rig->rail_loop = loop;
    return true;
}

void camera_rig_blend_from_here(CameraRig* rig, float seconds) {
    if (!rig) {
        log_error("camera_rig_blend_from_here: NULL rig");
        return;
    }
    // Nothing to blend out of before the first update, and a blend of no length
    // is a cut -- both are the same statement: leave the next pose alone.
    if (!rig->posed || seconds <= 0.0f) {
        rig->blend_left = 0.0f;
        return;
    }
    rig->blend_from = rig->pose;
    rig->blend_secs = seconds;
    rig->blend_left = seconds;
}

void camera_rig_shake(CameraRig* rig, float amplitude, float seconds) {
    if (!rig) {
        log_error("camera_rig_shake: NULL rig");
        return;
    }
    if (seconds <= 0.0f || amplitude <= 0.0f) {
        rig->shake_left = 0.0f;
        return;
    }
    rig->shake_amp = amplitude;
    rig->shake_secs = seconds;
    rig->shake_left = seconds;
}

void camera_rig_set_pose(CameraRig* rig, const vec3 eye, const vec3 look) {
    if (!rig || !eye || !look) {
        log_error("camera_rig_set_pose: NULL argument");
        return;
    }

    vec3 arm;
    glm_vec3_sub((float*)look, (float*)eye, arm);
    const float dist = glm_vec3_norm(arm);

    rig->look_lift = 0.0f;
    rig->near_eye_lift = rig->far_eye_lift = rig->eye_lift = 0.0f;
    glm_vec3_copy((float*)look, rig->anchor);

    if (dist > RIG_FIRST_PERSON_EPS) {
        vec3 dir;
        glm_vec3_divs(arm, dist, dir);
        camera_rig_set_distance(rig, dist);
        // NOT clamped to [pitch_min, pitch_max], unlike every other way the aim
        // moves. A stated pose is stated: --cam-eye exists to reproduce a view
        // exactly, and a rig quietly pulling it back inside a band it was given
        // for steering would make an exact framing impossible to ask for. The
        // clamp belongs where a pitch is being CHANGED, which is camera_rig_aim
        // and the rate path. A later drag re-enters the band on its own.
        rig->pitch = asinf(glm_clamp(dir[1], -1.0f, 1.0f));
        rig->yaw = atan2f(dir[0], dir[2]);
    } else {
        // An eye on its own target states no direction, so the aim it already
        // had is the only answer there is.
        camera_rig_set_distance(rig, 0.0f);
    }

    glm_vec3_copy((float*)eye, rig->stated.eye);
    glm_vec3_copy((float*)look, rig->stated.look);
    rig->base = rig->stated;
    rig->pose = rig->stated;
    CameraRigPose derived;
    _rig_derive(rig, &derived);
    glm_vec3_copy(derived.eye, rig->stated_derive);
    rig->pose_stated = true;
    rig->posed = true;
}

void camera_rig_apply(const CameraRig* rig, Camera* camera) {
    if (!camera) {
        log_error("camera_rig_apply: NULL camera");
        return;
    }
    // A NULL rig is silent, and that is a contract rather than leniency: "no rig
    // installed" is the normal state of an app that poses its own camera, and it
    // is what the engine's frame loop passes every frame in one. Making it an
    // error would have put a guard back at the one call site this exists to take
    // a guard away from. An unposed rig is the same statement one step later.
    if (!rig || !rig->posed)
        return;
    camera_set_position(camera, (float*)rig->pose.eye);
    camera_set_look_at(camera, (float*)rig->pose.look);
}

bool camera_rig_move_basis(const CameraRig* rig, float* out_yaw) {
    if (!rig || !rig->steers_controls)
        return false;
    if (out_yaw)
        *out_yaw = rig->yaw;
    return true;
}
