#include <cglm/cglm.h>
#include <stdlib.h>

#include "camera.h"

#include "camera.h"

// Function to create and initialize a Camera object
Camera* create_camera() {
    Camera* camera = (Camera*)malloc(sizeof(Camera));
    if (!camera) {
        return NULL;
    }

    camera->name = NULL;

    glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, camera->position);
    glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, camera->look_at);
    glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, camera->up);
    camera->fov = 45.0f;
    camera->aspect_ratio = 16.0f / 9.0f;
    camera->near_clip = 0.1f;           
    camera->far_clip = 100.0f;          

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

void set_camera_up(Camera* camera, vec3 up) {
    if (camera) {
        glm_vec3_copy(up, camera->up);
    }
}

void set_camera_fov(Camera* camera, float fov) {
    if (camera) {
        camera->fov = fov;
    }
}

void set_camera_aspect_ratio(Camera* camera, float aspect_ratio) {
    if (camera) {
        camera->aspect_ratio = aspect_ratio;
    }
}

void set_camera_clip_planes(Camera* camera, float near_clip, float far_clip) {
    if (camera) {
        camera->near_clip = near_clip;
        camera->far_clip = far_clip;
    }
}


