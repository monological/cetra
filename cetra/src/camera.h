
#ifndef _CAMERA_H_
#define _CAMERA_H_

#include <cglm/cglm.h>
#include <stdbool.h>
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

    bool is_orthographic; // true = parallel projection; fov_radians is then unused
    float ortho_height;   // World-space height of the ortho view volume; width is
                          // ortho_height * aspect_ratio

    // for animation
    float theta;
    float phi;
    float distance;
    float max_distance; // Max distance from look_at (0 = unlimited)
    float height;
    float zoom_speed;
    float orbit_speed;
    float amplitude;
} Camera;

// What a camera is created from. Fill the fields you mean with designated
// initialisers and leave the rest zero: zero is the default, named beside each
// field. A vec3 left zero is the default too, which is why look_at defaults to
// the origin and position does not (zero would put the eye at its target).
//
// ortho_height above zero makes the camera orthographic over a view volume that
// tall (width follows the aspect); fov is then unused but kept for a switch
// back. A 2D view needs ortho: under perspective, any camera rotation shears
// flat geometry. Scope: the projection, ray picking, the post stack, clustered
// light culling, LOD, occlusion culling, the cascade fit and the PBR view
// vector all honour it (spec 11.104). What still assumes a perspective
// frustum: the skybox cube and sky background, the ocean's projected grid, the
// cloud march, the depth-sort key and the gizmo's world-per-pixel.
typedef struct CameraDesc {
    const char* name;   // copied; NULL = unnamed
    vec3 position;      // 0 = (0, 2, 5)
    vec3 look_at;       // the origin
    vec3 up;            // 0 = +Y
    float fov;          // radians; 0 = 60 degrees
    float near;         // 0 = 0.1
    float far;          // 0 = 1000
    float ortho_height; // 0 = perspective
} CameraDesc;

// NULL means every default. The orbit parameters (distance, theta, phi) are
// derived from the pose once, here; the orbit tuning (max_distance, the two
// speeds) and everything else on a Camera is a plain field.
Camera* create_camera(const CameraDesc* desc);
void free_camera(Camera* camera);

// The pose. Functions so that the orbit parameters can follow a pose change;
// today they store, and a caller that moves the camera by pose and then
// orbits it re-derives distance, theta and phi itself.
void camera_set_position(Camera* camera, vec3 position);
void camera_set_look_at(Camera* camera, vec3 look_at);

// The view volume's height when the camera is orthographic, else 0. A camera switched
// back to perspective keeps its ortho_height for the switch back, so "0 unless
// orthographic" is read through this rather than off the field.
static inline float camera_ortho_height(const Camera* camera) {
    return camera->is_orthographic ? camera->ortho_height : 0.0f;
}

// Camera movement helpers
void camera_orbit(Camera* camera, float delta_theta, float delta_phi);
void camera_pan(Camera* camera, float delta_x, float delta_y);
void camera_zoom(Camera* camera, float delta);
void camera_move_forward(Camera* camera, float distance);
void camera_strafe(Camera* camera, float distance);
void camera_move_up(Camera* camera, float distance);
void camera_zoom_toward_target(Camera* camera, float factor, float min_distance);

// Pull the camera back to max_distance along its own view ray (no-op when
// max_distance is 0). Clamping toward the origin instead would make the
// camera slide around the boundary sphere.
void camera_enforce_max_distance(Camera* camera);

// Sync spherical coordinates from current position
void camera_sync_spherical_from_position(Camera* camera);

// Matrix computation
void camera_view_matrix(Camera* camera, mat4 view);
void camera_projection_matrix(const Camera* camera, mat4 projection);

#endif // _CAMERA_H_
