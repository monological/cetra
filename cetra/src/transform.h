#ifndef TRANSFORM_H
#define TRANSFORM_H

#include <cglm/cglm.h>

typedef struct {
    vec3 position;
    vec3 rotation;
    vec3 scale;
} Transform;

void transform_apply(mat4* matrix, Transform* transform);

void transform_set_position(Transform* transform, vec3 position);
void transform_set_rotation(Transform* transform, vec3 rotation);
void transform_set_scale(Transform* transform, vec3 scale);

void transform_add_position(Transform* transform, vec3 position);
void transform_add_rotation(Transform* transform, vec3 rotation);
void transform_add_scale(Transform* transform, vec3 scale);

#endif // TRANSFORM_H
