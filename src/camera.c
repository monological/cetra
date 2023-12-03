#include <cglm/cglm.h>
#include <stdlib.h>

#include "camera.h"

// Function to create and initialize a Camera object
Camera* create_camera() {
    Camera* camera = (Camera*)malloc(sizeof(Camera));
    if (!camera) {
        return NULL;
    }

    camera->name = NULL;

    glm_vec3_copy((vec3){0.0f, 2.0f, 5.0f}, camera->position); // Default position
    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, camera->look_at);   // Looking towards the origin
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, camera->up_vector);       // 'Up' is in the Y direction
    camera->aspect_ratio = 16.0f / 9.0f;
    camera->fov_radians = glm_rad(60.0f);  // A typical field of view
    camera->near_clip = 0.1f;              // Typical near clip plane
    camera->far_clip = 1000.0f;            // Typical far clip plane

    // animation variables
    camera->theta = 0.0f;
    camera->phi = 0.0f;
    camera->distance = 2000.0f;
    camera->height = 0.0f;
    camera->zoom_speed = 0.005f;
    camera->orbit_speed = 0.001f;
    camera->amplitude = 0.001f;

    return camera;
}

// Function to free a Camera object
void free_camera(Camera* camera) {
    if (camera) {
        if (camera->name) {
            free(camera->name);
        }
        free(camera);
    }
}

// Setters for Camera properties
void set_camera_position(Camera* camera, vec3 position) {
    if (camera) {
        glm_vec3_copy(position, camera->position);
    }
}

void set_camera_look_at(Camera* camera, vec3 look_at) {
    if (camera) {
        glm_vec3_copy(look_at, camera->look_at);
    }
}

void set_camera_up_vector(Camera* camera, vec3 up_vector) {
    if (camera) {
        glm_vec3_copy(up_vector, camera->up_vector);
    }
}

void set_camera_perspective(Camera* camera, float fov_degrees, float near_clip, float far_clip){
    if (!camera) return;
    camera->fov_radians = glm_rad(fov_degrees);
    camera->near_clip = near_clip;
    camera->far_clip = far_clip;
}


