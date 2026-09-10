
#ifndef _CAMERA_H_
#define _CAMERA_H_

/*
 * A camera: a pose (eye and target), a projection (perspective by field of
 * view, or orthographic by view-volume height), and the same pose expressed
 * as an orbit about the target (distance, theta, phi) for the controllers
 * that move it that way (specs 11.106, 11.107).
 *
 * Created from a CameraDesc; NULL means every default. The engine derives
 * the view and projection matrices itself, right after the app's pre_render
 * hook, so an app writes the pose and nothing else, and the aspect follows
 * the framebuffer every frame. The pose and the orbit describe each other,
 * and the two pose setters below say how that is kept.
 */

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct Camera {
    // ENGINE-OWNED: derived state. Read freely, never write.
    char* name;         // Copied at creation; freed with the camera
    float aspect_ratio; // Re-derived from the framebuffer every frame

    // BY FUNCTION: through, or followed by, the function named.
    vec3 position; // camera_set_position
    vec3 look_at;  // camera_set_look_at
    // The pose as an orbit about look_at. Written directly, then
    // camera_orbit(c, 0, 0) places the eye from them; the orbit moves do both.
    float theta;
    float phi;
    float distance;

    // SETTINGS: plain stores. Write them directly, at any time.
    vec3 up_vector;       // The view's up; +Y by default
    float fov_radians;    // Vertical field of view
    float near_clip;      // Near clipping plane
    float far_clip;       // Far clipping plane
    bool is_orthographic; // true = parallel projection; fov_radians is then unused
    float ortho_height;   // World-space height of the ortho view volume; width is
                          // ortho_height * aspect_ratio
    float max_distance;   // Max distance from look_at (0 = unlimited)
    float zoom_speed;     // Distance per unit of camera_zoom's delta
    float orbit_speed;    // Radians of phi per frame under an auto-orbit
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

// NULL means every default. The orbit parameters are derived from the pose.
Camera* create_camera(const CameraDesc* desc);
void free_camera(Camera* camera);

// The pose. Functions because the orbit parameters follow it: each re-derives
// distance, theta and phi, so a camera moved by pose and then orbited continues
// from where it is. The converse holds too: a write to the three directly is
// followed by camera_orbit(camera, 0, 0), which moves the eye onto them. A
// direct write to the pose alone leaves the orbit stale, which is the whole
// reason these two are not fields.
void camera_set_position(Camera* camera, vec3 position);
void camera_set_look_at(Camera* camera, vec3 look_at);

// The unit view direction, normalize(look_at - position). The camera stores a
// look point, not a forward vector, so this is the one derivation of it.
void camera_forward(const Camera* camera, vec3 out);

// The view volume's height when the camera is orthographic, else 0. A camera switched
// back to perspective keeps its ortho_height for the switch back, so "0 unless
// orthographic" is read through this rather than off the field.
static inline float camera_ortho_height(const Camera* camera) {
    return camera->is_orthographic ? camera->ortho_height : 0.0f;
}

// The moves. camera_translate slides eye and target by one world vector, which
// leaves the orbit as it is; the others are built on it or on camera_orbit.
void camera_translate(Camera* camera, const vec3 offset);
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

// Matrix computation
void camera_view_matrix(Camera* camera, mat4 view);
void camera_projection_matrix(const Camera* camera, mat4 projection);

#endif // _CAMERA_H_
