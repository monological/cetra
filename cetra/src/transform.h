#ifndef TRANSFORM_H
#define TRANSFORM_H

#include <cglm/cglm.h>

typedef struct {
    vec3 position;
    vec3 rotation;
    vec3 scale;
} Transform;

void reset_and_apply_transform(mat4* matrix, Transform* transform);

void set_transform_position(Transform* transform, vec3 position);
void set_transform_rotation(Transform* transform, vec3 rotation);
void set_transform_scale(Transform* transform, vec3 scale);

void update_transform_position(Transform* transform, vec3 position);
void update_transform_rotation(Transform* transform, vec3 rotation);
void update_transform_scale(Transform* transform, vec3 scale);

#endif // TRANSFORM_H
