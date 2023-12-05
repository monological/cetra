
#ifndef _CAMERA_H_
#define _CAMERA_H_

#include <cglm/cglm.h>
#include <stdlib.h>

typedef struct Camera {
    char* name;

    vec3 position;        // Camera position
    vec3 up_vector;       // up_vector vector
    vec3 look_at;         // Look-at point

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

#endif // _CAMERA_H_

