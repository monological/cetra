#include <cglm/cglm.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "camera.h"

// distance, theta and phi from the pose. Every write to the pose ends here and
// every orbit move writes the pose from these, so the two never disagree; a
// caller that writes the three directly follows with camera_orbit(camera, 0, 0)
// to move the eye onto them.
static void _camera_sync_orbit(Camera* camera) {
    vec3 to_camera;
    glm_vec3_sub(camera->position, camera->look_at, to_camera);
    float dist = glm_vec3_norm(to_camera);

    if (dist < 0.001f)
        dist = 1000.0f;

    camera->distance = dist;
    // Clamped: rounding can put |y| a ulp past the norm, and asinf of that is NaN.
    camera->theta = asinf(glm_clamp(to_camera[1] / dist, -1.0f, 1.0f));
    camera->phi = atan2f(to_camera[2], to_camera[0]);
}

Camera* create_camera(const CameraDesc* desc) {
    static const CameraDesc none = {0};
    if (!desc)
        desc = &none;

    Camera* camera = calloc(1, sizeof(Camera));
    if (!camera)
        return NULL;

    camera->name = safe_strdup(desc->name);

    // The eye's default is not the origin, which is why position takes a
    // fallback and look_at does not.
    vec3_or_default(desc->position, (vec3){0.0f, 2.0f, 5.0f}, camera->position);
    glm_vec3_copy((float*)desc->look_at, camera->look_at);
    vec3_or_default(desc->up, (vec3){0.0f, 1.0f, 0.0f}, camera->up_vector);

    camera->aspect_ratio = 16.0f / 9.0f; // re-derived from the framebuffer each frame
    camera->fov_radians = desc->fov > 0.0f ? desc->fov : glm_rad(60.0f);
    camera->near_clip = desc->near > 0.0f ? desc->near : 0.1f;
    camera->far_clip = desc->far > 0.0f ? desc->far : 1000.0f;
    camera->is_orthographic = desc->ortho_height > 0.0f;
    camera->ortho_height = desc->ortho_height;

    // The orbit parameters describe the pose just set, not a fixed 2000 units
    // that every orbiting app then had to overwrite by hand.
    _camera_sync_orbit(camera);
    return camera;
}

void free_camera(Camera* camera) {
    if (!camera)
        return;
    if (camera->name) {
        free(camera->name);
    }
    free(camera);
}

void camera_set_position(Camera* camera, vec3 position) {
    if (!camera)
        return;
    glm_vec3_copy(position, camera->position);
    _camera_sync_orbit(camera);
}

void camera_set_look_at(Camera* camera, vec3 look_at) {
    if (!camera)
        return;
    glm_vec3_copy(look_at, camera->look_at);
    _camera_sync_orbit(camera);
}

void camera_forward(const Camera* camera, vec3 out) {
    if (!camera) {
        glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, out); // a sane default, and out is always written
        return;
    }
    glm_vec3_sub((float*)camera->look_at, (float*)camera->position, out);
    glm_vec3_normalize(out);
}

void camera_translate(Camera* camera, const vec3 offset) {
    if (!camera)
        return;
    // Eye and target by one vector: the view direction, the distance and the
    // two angles are unchanged, so nothing here needs re-deriving.
    glm_vec3_add(camera->position, (float*)offset, camera->position);
    glm_vec3_add(camera->look_at, (float*)offset, camera->look_at);
}

void camera_view_matrix(Camera* camera, mat4 view) {
    if (!camera)
        return;
    glm_lookat(camera->position, camera->look_at, camera->up_vector, view);
}

void camera_projection_matrix(const Camera* camera, mat4 projection) {
    if (!camera)
        return;

    if (camera->is_orthographic) {
        float half_h = camera->ortho_height * 0.5f;
        float half_w = half_h * camera->aspect_ratio;
        glm_ortho(-half_w, half_w, -half_h, half_h, camera->near_clip, camera->far_clip,
                  projection);
        return;
    }

    glm_perspective(camera->fov_radians, camera->aspect_ratio, camera->near_clip, camera->far_clip,
                    projection);
}

// A fitted camera's margins (spec 13.57): far past the farthest box, near short of the nearest.
#define CLIP_FIT_FAR_MARGIN  1.1f
#define CLIP_FIT_NEAR_MARGIN 0.9f

void camera_clip_fit_begin(CameraClipFit* fit, const Camera* camera) {
    *fit = (CameraClipFit){.nearest_depth = FLT_MAX, .nearest_dist_sq = FLT_MAX};
    glm_vec3_copy((float*)camera->position, fit->eye);
    camera_forward(camera, fit->forward);
}

void camera_clip_fit_add(CameraClipFit* fit, const AABB* box) {
    // A box's depths span its centre's depth plus or minus its half-extent along |forward|.
    vec3 centre, half, rel, reach;
    glm_vec3_add((float*)box->min, (float*)box->max, centre);
    glm_vec3_scale(centre, 0.5f, centre);
    glm_vec3_sub((float*)box->max, (float*)box->min, half);
    glm_vec3_scale(half, 0.5f, half);
    glm_vec3_sub(centre, fit->eye, rel);
    glm_vec3_abs(fit->forward, reach);
    const float mid = glm_vec3_dot(rel, fit->forward);
    const float spread = glm_vec3_dot(half, reach);
    const float lo = mid - spread, hi = mid + spread;
    if (hi <= 0.0f)
        return;
    fit->nearest_depth = fminf(fit->nearest_depth, fmaxf(lo, 0.0f));
    fit->nearest_dist_sq = fminf(fit->nearest_dist_sq, aabb_dist_sq(box, fit->eye));
    fit->farthest_depth = fmaxf(fit->farthest_depth, hi);
}

void camera_clip_fit_apply(Camera* camera, const CameraClipFit* fit) {
    if (!(fit->farthest_depth > 0.0f))
        return;
    camera->far_clip = fit->farthest_depth * CLIP_FIT_FAR_MARGIN;
    float nearest = fit->nearest_depth;
    if (!camera->is_orthographic) {
        // Inside the frustum a point's depth is at least its distance times the cosine of the
        // frustum's corner ray, so the distance is a second bound, and the one that holds where a
        // box reaching behind the eye takes the first to zero. Past the farthest depth it says no
        // box reaches the frustum at all, and binds nothing.
        const float k = tanf(0.5f * camera->fov_radians);
        const float k2 = k * k * (1.0f + camera->aspect_ratio * camera->aspect_ratio);
        const float bound = sqrtf(fit->nearest_dist_sq) / sqrtf(1.0f + k2);
        if (bound <= fit->farthest_depth)
            nearest = fmaxf(nearest, bound);
    }
    camera->near_clip =
        fmaxf(nearest * CLIP_FIT_NEAR_MARGIN, camera->far_clip / CAMERA_CLIP_RATIO_MAX);
}
