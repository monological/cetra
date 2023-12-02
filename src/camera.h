
#ifndef _CAMERA_H_
#define _CAMERA_H_

#include <cglm/cglm.h>
#include <stdlib.h>

typedef struct Camera {
    char* name;
    vec3 position;    // Camera position
    vec3 up;          // Up vector
    vec3 look_at;      // Look-at point
    float fov;        // Field of view (in radians)
    float aspect_ratio;// Aspect ratio
    float near_clip; // Near clipping plane
    float far_clip;  // Far clipping plane
    float horizontal_fov; // Horizontal field of view (in radians)
} Camera;


Camera* create_camera();
void free_camera(Camera* camera);
void set_camera_position(Camera* camera, vec3 position);
void set_camera_direction(Camera* camera, vec3 direction);
void set_camera_up(Camera* camera, vec3 up);
void set_camera_fov(Camera* camera, float fov);
void set_camera_aspect_ratio(Camera* camera, float aspectRatio);
void set_camera_clip_planes(Camera* camera, float nearClip, float farClip);


#endif // _CAMERA_H_

