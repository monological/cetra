
#ifndef _CAMERA_H_
#define _CAMERA_H_

#include <cglm/cglm.h>
#include <stdlib.h>

typedef struct Camera {
    char* name;

    vec3 position;  // Camera position
    vec3 up_vector; // up_vector vector
    vec3 look_at;   // Look-at point

    float fov_radians;    // Field of view (in radians)
    float aspect_ratio;   // Aspect ratio
    float near_clip;      // Near clipping plane
    float far_clip;       // Far clipping plane
    float horizontal_fov; // Horizontal field of view (in radians)

    // for animation
    float theta;
    float phi;
    float distance;
    float height;
    float zoom_speed;
    float orbit_speed;
    float amplitude;
} Camera;

Camera* create_camera();
void free_camera(Camera* camera);

void set_camera_name(Camera* camera, const char* name);
void set_camera_position(Camera* camera, vec3 position);
void set_camera_look_at(Camera* camera, vec3 look_at);
void set_camera_direction(Camera* camera, vec3 direction);
void set_camera_up_vector(Camera* camera, vec3 up_vector);
void set_camera_perspective(Camera* camera, float fov_radians, float near_clip, float far_clip);

// Camera movement helpers
void orbit_camera(Camera* camera, float delta_theta, float delta_phi);
void pan_camera(Camera* camera, float delta_x, float delta_y);
void zoom_camera(Camera* camera, float delta);
void camera_move_forward(Camera* camera, float distance);
void camera_strafe(Camera* camera, float distance);
void camera_move_up(Camera* camera, float distance);
void camera_zoom_toward_target(Camera* camera, float factor, float min_distance);

// Sync spherical coordinates from current position
void camera_sync_spherical_from_position(Camera* camera);

// Matrix computation
void compute_view_matrix(Camera* camera, mat4 view);
void compute_projection_matrix(const Camera* camera, mat4 projection);

#endif // _CAMERA_H_
