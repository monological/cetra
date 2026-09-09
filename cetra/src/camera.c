#include <cglm/cglm.h>
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

    camera->zoom_speed = 0.005f;
    camera->orbit_speed = 0.001f;

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

void camera_orbit(Camera* camera, float delta_theta, float delta_phi) {
    if (!camera)
        return;

    camera->theta += delta_theta;
    camera->phi += delta_phi;

    // Clamp theta (vertical/elevation) to avoid flipping at poles
    // theta: -pi/2 (looking down) to +pi/2 (looking up)
    float limit = GLM_PI_2f - 0.1f;
    if (camera->theta > limit)
        camera->theta = limit;
    if (camera->theta < -limit)
        camera->theta = -limit;

    if (camera->max_distance > 0.0f && camera->distance > camera->max_distance)
        camera->distance = camera->max_distance;

    // Update position from spherical coordinates
    // theta = elevation angle (vertical), phi = azimuth angle (horizontal)
    float cos_theta = cosf(camera->theta);
    camera->position[0] = camera->look_at[0] + camera->distance * cos_theta * cosf(camera->phi);
    camera->position[1] = camera->look_at[1] + camera->distance * sinf(camera->theta);
    camera->position[2] = camera->look_at[2] + camera->distance * cos_theta * sinf(camera->phi);
}

void camera_translate(Camera* camera, const vec3 offset) {
    if (!camera)
        return;
    // Eye and target by one vector: the view direction, the distance and the
    // two angles are unchanged, so nothing here needs re-deriving.
    glm_vec3_add(camera->position, (float*)offset, camera->position);
    glm_vec3_add(camera->look_at, (float*)offset, camera->look_at);
}

void camera_pan(Camera* camera, float delta_x, float delta_y) {
    if (!camera)
        return;

    // Compute forward direction (look_at - position)
    vec3 forward;
    glm_vec3_sub(camera->look_at, camera->position, forward);
    glm_vec3_normalize(forward);

    // Compute right vector (up x forward) - matches original engine.c convention
    vec3 right;
    glm_vec3_cross(camera->up_vector, forward, right);
    glm_vec3_normalize(right);

    // Along the right axis, then along the world up axis
    vec3 offset;
    glm_vec3_scale(right, delta_x, offset);
    camera_translate(camera, offset);
    glm_vec3_scale(camera->up_vector, delta_y, offset);
    camera_translate(camera, offset);
}

void camera_zoom(Camera* camera, float delta) {
    if (!camera)
        return;

    camera->distance += delta * camera->zoom_speed;
    if (camera->distance < 0.1f)
        camera->distance = 0.1f;

    // Recompute position from orbit parameters
    camera_orbit(camera, 0.0f, 0.0f);
}

void camera_move_forward(Camera* camera, float distance) {
    if (!camera)
        return;

    vec3 forward;
    glm_vec3_sub(camera->look_at, camera->position, forward);
    glm_vec3_normalize(forward);

    vec3 movement;
    glm_vec3_scale(forward, distance, movement);
    camera_translate(camera, movement);
}

void camera_strafe(Camera* camera, float distance) {
    if (!camera)
        return;

    vec3 forward;
    glm_vec3_sub(camera->look_at, camera->position, forward);

    vec3 right;
    glm_vec3_crossn(camera->up_vector, forward, right);
    glm_vec3_normalize(right);

    vec3 movement;
    glm_vec3_scale(right, distance, movement);
    camera_translate(camera, movement);
}

void camera_move_up(Camera* camera, float distance) {
    if (!camera)
        return;

    vec3 movement;
    glm_vec3_scale(camera->up_vector, distance, movement);
    camera_translate(camera, movement);
}

void camera_zoom_toward_target(Camera* camera, float factor, float min_distance) {
    if (!camera)
        return;

    vec3 to_camera;
    glm_vec3_sub(camera->position, camera->look_at, to_camera);
    float dist = glm_vec3_norm(to_camera);

    if (dist < 0.001f)
        dist = 1000.0f;

    float new_dist = dist * factor;
    if (new_dist < min_distance)
        new_dist = min_distance;
    if (camera->max_distance > 0.0f && new_dist > camera->max_distance)
        new_dist = camera->max_distance;

    glm_vec3_normalize(to_camera);
    glm_vec3_scale(to_camera, new_dist, to_camera);
    glm_vec3_add(camera->look_at, to_camera, camera->position);

    camera->distance = new_dist;
}

void camera_enforce_max_distance(Camera* camera) {
    if (!camera || camera->max_distance <= 0.0f)
        return;

    if (camera->distance > camera->max_distance)
        camera->distance = camera->max_distance;

    vec3 offset;
    glm_vec3_sub(camera->position, camera->look_at, offset);
    float dist = glm_vec3_norm(offset);

    if (dist > camera->max_distance && dist > 1e-6f) {
        glm_vec3_scale(offset, camera->max_distance / dist, offset);
        glm_vec3_add(camera->look_at, offset, camera->position);
    }
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
