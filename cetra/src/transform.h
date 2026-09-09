#ifndef TRANSFORM_H
#define TRANSFORM_H

#include <cglm/cglm.h>

// A pose as three vectors, filled directly; transform_apply composes it into a
// matrix as translate, then rotate about X, Y, Z in that order, then scale.
typedef struct {
    vec3 position;
    vec3 rotation; // Euler, radians
    vec3 scale;
} Transform;

void transform_apply(mat4* matrix, Transform* transform);

#endif // TRANSFORM_H
