#ifndef INTERSECT_H
#define INTERSECT_H

#include <stdbool.h>
#include <cglm/cglm.h>

// Forward declarations to avoid header dependency issues
struct SceneNode;

// Frustum with 6 planes (left, right, bottom, top, near, far)
// Each plane stored as vec4: (a, b, c, d) where ax + by + cz + d = 0
typedef struct {
    vec4 planes[6];
} Frustum;

// Frustum plane indices
#define FRUSTUM_LEFT   0
#define FRUSTUM_RIGHT  1
#define FRUSTUM_BOTTOM 2
#define FRUSTUM_TOP    3
#define FRUSTUM_NEAR   4
#define FRUSTUM_FAR    5

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

// Extract frustum planes from view-projection matrix (Griggs-Hartmann method)
void frustum_extract_from_vp(mat4 vp, Frustum* frustum);

// Test if AABB (in local space) transformed by model matrix is visible in frustum
// Returns true if AABB is inside or intersects frustum, false if completely outside
bool frustum_test_aabb_transformed(const Frustum* frustum, vec3 aabb_min, vec3 aabb_max,
                                   mat4 model);

// Transform AABB to world space (computes world-space AABB from local AABB and transform)
void aabb_transform(vec3 aabb_min, vec3 aabb_max, mat4 transform, vec3 out_min, vec3 out_max);

#endif // INTERSECT_H
