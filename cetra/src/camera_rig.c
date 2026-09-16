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
    rig->dist = 5.0f;
    rig->pitch_min = -1.5f;
    rig->pitch_max = 1.5f;
    // apps/gametest's LOOK_YAW_RATE and LOOK_PITCH_RATE, which were apps/forest's
    // written a second time.
    rig->yaw_rate = 1.8f;
    rig->pitch_rate = 1.2f;
    return rig;
}

void free_camera_rig(CameraRig* rig) {
    free(rig);
}

// The aim direction. One derivation, where there were three.
static void _rig_dir(const CameraRig* rig, vec3 out) {
    const float cp = cosf(rig->pitch);
    out[0] = sinf(rig->yaw) * cp;
    out[1] = sinf(rig->pitch);
    out[2] = cosf(rig->yaw) * cp;
}

void camera_rig_update(CameraRig* rig, float dt, float yaw_in, float pitch_in) {
    if (!rig) {
        log_error("camera_rig_update: NULL rig");
        return;
    }

    rig->yaw -= yaw_in * rig->yaw_rate * dt;
    rig->pitch += pitch_in * rig->pitch_rate * dt;
    rig->pitch = glm_clamp(rig->pitch, rig->pitch_min, rig->pitch_max);

    vec3 dir = {0.0f, 0.0f, 0.0f};
    _rig_dir(rig, dir);

    vec3 look;
    glm_vec3_copy(rig->anchor, look);
    look[1] += rig->look_lift;

    vec3 eye;
    glm_vec3_copy(look, eye);
    eye[1] += rig->eye_lift;
    glm_vec3_mulsubs(dir, rig->dist, eye);

    glm_vec3_copy(eye, rig->pose.eye);
    if (rig->dist < RIG_FIRST_PERSON_EPS) {
        glm_vec3_add(eye, dir, rig->pose.look);
    } else {
        glm_vec3_copy(look, rig->pose.look);
    }
    rig->posed = true;
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
    rig->eye_lift = 0.0f;
    glm_vec3_copy((float*)look, rig->anchor);

    if (dist > RIG_FIRST_PERSON_EPS) {
        vec3 dir;
        glm_vec3_divs(arm, dist, dir);
        rig->dist = dist;
        rig->pitch =
            glm_clamp(asinf(glm_clamp(dir[1], -1.0f, 1.0f)), rig->pitch_min, rig->pitch_max);
        rig->yaw = atan2f(dir[0], dir[2]);
    } else {
        // An eye on its own target states no direction, so the aim it already
        // had is the only answer there is.
        rig->dist = 0.0f;
    }

    glm_vec3_copy((float*)eye, rig->pose.eye);
    glm_vec3_copy((float*)look, rig->pose.look);
    rig->posed = true;
}

void camera_rig_apply(const CameraRig* rig, Camera* camera) {
    if (!rig || !camera) {
        log_error("camera_rig_apply: NULL rig or camera");
        return;
    }
    if (!rig->posed)
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
