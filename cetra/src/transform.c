
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "transform.h"

void reset_and_apply_transform(mat4* matrix, Transform* transform) {
    if (!transform || !matrix) {
        glm_mat4_identity(*matrix);
        return;
    }

    glm_mat4_identity(*matrix);
    glm_translate(*matrix, transform->position);
    glm_rotate(*matrix, transform->rotation[0], (vec3){1.0f, 0.0f, 0.0f});
    glm_rotate(*matrix, transform->rotation[1], (vec3){0.0f, 1.0f, 0.0f});
    glm_rotate(*matrix, transform->rotation[2], (vec3){0.0f, 0.0f, 1.0f});
    glm_scale(*matrix, transform->scale);
}

void set_transform_position(Transform* transform, vec3 position) {
    if (!transform) {
        return;
    }

    glm_vec3_copy(position, transform->position);
}

void set_transform_rotation(Transform* transform, vec3 rotation) {
    if (!transform) {
        return;
    }

    glm_vec3_copy(rotation, transform->rotation);
}

void set_transform_scale(Transform* transform, vec3 scale) {
    if (!transform) {
        return;
    }

    glm_vec3_copy(scale, transform->scale);
}

void update_transform_position(Transform* transform, vec3 position) {
    if (!transform) {
        return;
    }

    glm_vec3_add(transform->position, position, transform->position);
}

void update_transform_rotation(Transform* transform, vec3 rotation) {
    if (!transform) {
        return;
    }

    glm_vec3_add(transform->rotation, rotation, transform->rotation);
}

void update_transform_scale(Transform* transform, vec3 scale) {
    if (!transform) {
        return;
    }

    glm_vec3_add(transform->scale, scale, transform->scale);
}
