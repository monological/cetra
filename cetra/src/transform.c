
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "transform.h"

void transform_apply(mat4* matrix, Transform* transform) {
    if (!matrix) {
        return;
    }
    if (!transform) {
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
