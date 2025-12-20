#ifndef INTERSECT_H
#define INTERSECT_H

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations to avoid header dependency issues
struct SceneNode;

typedef struct {
    struct SceneNode* node;
    float distance;
    vec3 hit_point;
    bool hit;
} RayPickResult;

// Compute ray direction from screen coordinates
void compute_ray_from_screen(float screen_x, float screen_y, int fb_width, int fb_height,
                             mat4 projection, mat4 view, vec3 ray_origin, vec3 out_ray_dir);

// Ray-AABB intersection test
bool ray_aabb_intersection(vec3 ray_origin, vec3 ray_dir, vec3 bbox_min, vec3 bbox_max,
                           float* t_near, float* t_far);

// Pick scene node under ray (traverses scene graph, tests AABB then triangles)
RayPickResult pick_scene_node(struct SceneNode* root_node, vec3 ray_origin, vec3 ray_dir);

// Project ray to plane at given distance (for drag operations)
void ray_point_at_distance(vec3 ray_origin, vec3 ray_dir, float distance, vec3 out_point);

#endif // INTERSECT_H
