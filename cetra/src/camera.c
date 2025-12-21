#include <cglm/cglm.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "camera.h"

Camera* create_camera() {
    Camera* camera = (Camera*)malloc(sizeof(Camera));
    if (!camera) {
        return NULL;
    }

    camera->name = NULL;

    glm_vec3_copy((vec3){0.0f, 2.0f, 5.0f}, camera->position);  // Default position
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, camera->look_at);   // Looking towards the origin
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, camera->up_vector); // 'Up' is in the Y direction

    camera->aspect_ratio = 16.0f / 9.0f;
    camera->fov_radians = glm_rad(60.0f); // A typical field of view
    camera->near_clip = 0.1f;             // Typical near clip plane
    camera->far_clip = 1000.0f;           // Typical far clip plane

    // orbit animation variables
    camera->theta = 0.0f;
    camera->phi = 0.0f;
    camera->distance = 2000.0f;
    camera->height = 0.0f;
    camera->zoom_speed = 0.005f;
    camera->orbit_speed = 0.001f;
    camera->amplitude = 0.001f;

    return camera;
}

void set_camera_name(Camera* camera, const char* name) {
    if (!camera)
        return;
    if (camera->name) {
        free(camera->name);
    }
    camera->name = safe_strdup(name);
}

void free_camera(Camera* camera) {
    if (!camera)
        return;
    if (camera->name) {
        free(camera->name);
    }
    free(camera);
}

void set_camera_position(Camera* camera, vec3 position) {
    if (!camera)
        return;
    glm_vec3_copy(position, camera->position);
}

void set_camera_look_at(Camera* camera, vec3 look_at) {
    if (!camera)
        return;
    glm_vec3_copy(look_at, camera->look_at);
}

void set_camera_direction(Camera* camera, vec3 direction) {
    if (!camera)
        return;
    glm_vec3_add(camera->position, direction, camera->look_at);
}

void set_camera_up_vector(Camera* camera, vec3 up_vector) {
    if (!camera)
        return;
    glm_vec3_copy(up_vector, camera->up_vector);
}

void set_camera_perspective(Camera* camera, float fov_radians, float near_clip, float far_clip) {
    if (!camera)
        return;
    camera->fov_radians = fov_radians;
    camera->near_clip = near_clip;
    camera->far_clip = far_clip;
}

void orbit_camera(Camera* camera, float delta_theta, float delta_phi) {
    if (!camera)
        return;

    camera->theta += delta_theta;
    camera->phi += delta_phi;

    // Clamp phi to avoid flipping at poles
    float limit = GLM_PI_2f - 0.01f;
    if (camera->phi > limit)
        camera->phi = limit;
    if (camera->phi < -limit)
        camera->phi = -limit;

    // Update position from spherical coordinates
    float cos_phi = cosf(camera->phi);
    camera->position[0] = camera->look_at[0] + camera->distance * cos_phi * sinf(camera->theta);
    camera->position[1] = camera->look_at[1] + camera->distance * sinf(camera->phi);
    camera->position[2] = camera->look_at[2] + camera->distance * cos_phi * cosf(camera->theta);
}

void pan_camera(Camera* camera, float delta_x, float delta_y) {
    if (!camera)
        return;

    // Compute forward direction (look_at - position)
    vec3 forward;
    glm_vec3_sub(camera->look_at, camera->position, forward);
    glm_vec3_normalize(forward);

    // Compute right vector (forward x up)
    vec3 right;
    glm_vec3_cross(forward, camera->up_vector, right);
    glm_vec3_normalize(right);

    // Compute actual up vector (right x forward)
    vec3 up;
    glm_vec3_cross(right, forward, up);

    // Move camera and target together
    vec3 offset;
    glm_vec3_scale(right, delta_x, offset);
    glm_vec3_add(camera->position, offset, camera->position);
    glm_vec3_add(camera->look_at, offset, camera->look_at);

    glm_vec3_scale(up, delta_y, offset);
    glm_vec3_add(camera->position, offset, camera->position);
    glm_vec3_add(camera->look_at, offset, camera->look_at);
}

void zoom_camera(Camera* camera, float delta) {
    if (!camera)
        return;

    camera->distance += delta * camera->zoom_speed;
    if (camera->distance < 0.1f)
        camera->distance = 0.1f;

    // Recompute position from orbit parameters
    orbit_camera(camera, 0.0f, 0.0f);
}

void compute_view_matrix(Camera* camera, mat4 view) {
    if (!camera)
        return;
    glm_lookat(camera->position, camera->look_at, camera->up_vector, view);
}

void compute_projection_matrix(Camera* camera, mat4 projection) {
    if (!camera)
        return;
    glm_perspective(camera->fov_radians, camera->aspect_ratio, camera->near_clip, camera->far_clip,
                    projection);
}
