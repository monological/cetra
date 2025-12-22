
#include <float.h>
#include <math.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "intersect.h"
#include "scene.h"
#include "mesh.h"
#include "ext/log.h"

void compute_ray_from_screen(float screen_x, float screen_y, int fb_width, int fb_height,
                             mat4 projection, mat4 view, vec3 ray_origin, vec3 out_ray_dir) {
    // Convert to NDC
    float x_ndc = (2.0f * screen_x) / fb_width - 1.0f;
    float y_ndc = (2.0f * screen_y) / fb_height - 1.0f;

    // Convert from NDC to eye space ray
    vec4 ray_clip = {x_ndc, y_ndc, -1.0f, 1.0f};
    vec4 ray_eye;
    mat4 inv_projection;
    glm_mat4_inv(projection, inv_projection);
    glm_mat4_mulv(inv_projection, ray_clip, ray_eye);

    // Transform ray to world space
    mat4 inv_view;
    glm_mat4_inv(view, inv_view);
    glm_mat4_mulv3(inv_view, ray_eye, 0.0f, out_ray_dir);
    glm_vec3_normalize(out_ray_dir);
}

bool ray_aabb_intersection(vec3 ray_origin, vec3 ray_dir, vec3 bbox_min, vec3 bbox_max,
                           float* t_near, float* t_far) {
    vec3 inv_dir = {1.0f / ray_dir[0], 1.0f / ray_dir[1], 1.0f / ray_dir[2]};
    vec3 t0s = {(bbox_min[0] - ray_origin[0]) * inv_dir[0],
                (bbox_min[1] - ray_origin[1]) * inv_dir[1],
                (bbox_min[2] - ray_origin[2]) * inv_dir[2]};
    vec3 t1s = {(bbox_max[0] - ray_origin[0]) * inv_dir[0],
                (bbox_max[1] - ray_origin[1]) * inv_dir[1],
                (bbox_max[2] - ray_origin[2]) * inv_dir[2]};

    vec3 tmin = {fminf(t0s[0], t1s[0]), fminf(t0s[1], t1s[1]), fminf(t0s[2], t1s[2])};
    vec3 tmax = {fmaxf(t0s[0], t1s[0]), fmaxf(t0s[1], t1s[1]), fmaxf(t0s[2], t1s[2])};

    *t_near = fmaxf(fmaxf(tmin[0], tmin[1]), tmin[2]);
    *t_far = fminf(fminf(tmax[0], tmax[1]), tmax[2]);

    return (*t_near <= *t_far) && (*t_far >= 0.0f);
}

static void traverse_and_pick(SceneNode* node, vec3 ray_origin, vec3 ray_dir, float* min_distance,
                              SceneNode** picked_node) {
    if (!node)
        return;

    mat4 inv_global_transform;
    glm_mat4_inv(node->global_transform, inv_global_transform);

    vec3 local_ray_origin, local_ray_dir;
    glm_mat4_mulv3(inv_global_transform, ray_origin, 1.0f, local_ray_origin);
    glm_mat4_mulv3(inv_global_transform, ray_dir, 0.0f, local_ray_dir);
    glm_vec3_normalize(local_ray_dir);

    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        float t_near, t_far;

        if (ray_aabb_intersection(local_ray_origin, local_ray_dir, mesh->aabb.min, mesh->aabb.max,
                                  &t_near, &t_far)) {
            if (t_near < *min_distance && t_near > 0) {
                // Test individual triangles for precise hit
                for (size_t j = 0; j < mesh->index_count; j += 3) {
                    vec3 v0, v1, v2;
                    glm_vec3_copy(mesh->vertices + mesh->indices[j] * 3, v0);
                    glm_vec3_copy(mesh->vertices + mesh->indices[j + 1] * 3, v1);
                    glm_vec3_copy(mesh->vertices + mesh->indices[j + 2] * 3, v2);

                    float t;
                    if (glm_ray_triangle(local_ray_origin, local_ray_dir, v0, v1, v2, &t) &&
                        t < *min_distance && t > 0) {
                        *min_distance = t;
                        *picked_node = node;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < node->children_count; i++) {
        traverse_and_pick(node->children[i], ray_origin, ray_dir, min_distance, picked_node);
    }
}

RayPickResult pick_scene_node(SceneNode* root_node, vec3 ray_origin, vec3 ray_dir) {
    RayPickResult result = {0};
    result.node = NULL;
    result.distance = FLT_MAX;
    result.hit = false;

    if (!root_node)
        return result;

    float min_distance = FLT_MAX;
    SceneNode* picked_node = NULL;

    traverse_and_pick(root_node, ray_origin, ray_dir, &min_distance, &picked_node);

    if (picked_node) {
        result.node = picked_node;
        result.distance = min_distance;
        result.hit = true;

        // Calculate hit point in world space
        glm_vec3_scale(ray_dir, min_distance, result.hit_point);
        glm_vec3_add(ray_origin, result.hit_point, result.hit_point);
    }

    return result;
}

void ray_point_at_distance(vec3 ray_origin, vec3 ray_dir, float distance, vec3 out_point) {
    glm_vec3_scale(ray_dir, distance, out_point);
    glm_vec3_add(ray_origin, out_point, out_point);
}

// Normalize a plane equation (a, b, c, d) so that (a, b, c) is a unit vector
static void normalize_plane(vec4 plane) {
    float length = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    if (length > 0.0f) {
        plane[0] /= length;
        plane[1] /= length;
        plane[2] /= length;
        plane[3] /= length;
    }
}

void frustum_extract_from_vp(mat4 vp, Frustum* frustum) {
    // Griggs-Hartmann method: extract planes from view-projection matrix
    // Each row of the VP matrix contributes to the plane equations
    // Note: cglm uses column-major, so vp[col][row]

    // Left plane: row3 + row0
    frustum->planes[FRUSTUM_LEFT][0] = vp[0][3] + vp[0][0];
    frustum->planes[FRUSTUM_LEFT][1] = vp[1][3] + vp[1][0];
    frustum->planes[FRUSTUM_LEFT][2] = vp[2][3] + vp[2][0];
    frustum->planes[FRUSTUM_LEFT][3] = vp[3][3] + vp[3][0];
    normalize_plane(frustum->planes[FRUSTUM_LEFT]);

    // Right plane: row3 - row0
    frustum->planes[FRUSTUM_RIGHT][0] = vp[0][3] - vp[0][0];
    frustum->planes[FRUSTUM_RIGHT][1] = vp[1][3] - vp[1][0];
    frustum->planes[FRUSTUM_RIGHT][2] = vp[2][3] - vp[2][0];
    frustum->planes[FRUSTUM_RIGHT][3] = vp[3][3] - vp[3][0];
    normalize_plane(frustum->planes[FRUSTUM_RIGHT]);

    // Bottom plane: row3 + row1
    frustum->planes[FRUSTUM_BOTTOM][0] = vp[0][3] + vp[0][1];
    frustum->planes[FRUSTUM_BOTTOM][1] = vp[1][3] + vp[1][1];
    frustum->planes[FRUSTUM_BOTTOM][2] = vp[2][3] + vp[2][1];
    frustum->planes[FRUSTUM_BOTTOM][3] = vp[3][3] + vp[3][1];
    normalize_plane(frustum->planes[FRUSTUM_BOTTOM]);

    // Top plane: row3 - row1
    frustum->planes[FRUSTUM_TOP][0] = vp[0][3] - vp[0][1];
    frustum->planes[FRUSTUM_TOP][1] = vp[1][3] - vp[1][1];
    frustum->planes[FRUSTUM_TOP][2] = vp[2][3] - vp[2][1];
    frustum->planes[FRUSTUM_TOP][3] = vp[3][3] - vp[3][1];
    normalize_plane(frustum->planes[FRUSTUM_TOP]);

    // Near plane: row3 + row2
    frustum->planes[FRUSTUM_NEAR][0] = vp[0][3] + vp[0][2];
    frustum->planes[FRUSTUM_NEAR][1] = vp[1][3] + vp[1][2];
    frustum->planes[FRUSTUM_NEAR][2] = vp[2][3] + vp[2][2];
    frustum->planes[FRUSTUM_NEAR][3] = vp[3][3] + vp[3][2];
    normalize_plane(frustum->planes[FRUSTUM_NEAR]);

    // Far plane: row3 - row2
    frustum->planes[FRUSTUM_FAR][0] = vp[0][3] - vp[0][2];
    frustum->planes[FRUSTUM_FAR][1] = vp[1][3] - vp[1][2];
    frustum->planes[FRUSTUM_FAR][2] = vp[2][3] - vp[2][2];
    frustum->planes[FRUSTUM_FAR][3] = vp[3][3] - vp[3][2];
    normalize_plane(frustum->planes[FRUSTUM_FAR]);
}

void aabb_transform(vec3 aabb_min, vec3 aabb_max, mat4 transform, vec3 out_min, vec3 out_max) {
    // Optimized AABB transformation using center/extents representation
    vec3 center, extents;

    // Compute center and half-extents
    glm_vec3_add(aabb_min, aabb_max, center);
    glm_vec3_scale(center, 0.5f, center);
    glm_vec3_sub(aabb_max, aabb_min, extents);
    glm_vec3_scale(extents, 0.5f, extents);

    // Transform center point
    vec4 center4 = {center[0], center[1], center[2], 1.0f};
    vec4 new_center4;
    glm_mat4_mulv(transform, center4, new_center4);

    // Compute new extents: for each axis, sum absolute contributions from original extents
    // This accounts for rotation stretching the AABB
    vec3 new_extents;
    new_extents[0] = fabsf(transform[0][0]) * extents[0] + fabsf(transform[1][0]) * extents[1] +
                     fabsf(transform[2][0]) * extents[2];
    new_extents[1] = fabsf(transform[0][1]) * extents[0] + fabsf(transform[1][1]) * extents[1] +
                     fabsf(transform[2][1]) * extents[2];
    new_extents[2] = fabsf(transform[0][2]) * extents[0] + fabsf(transform[1][2]) * extents[1] +
                     fabsf(transform[2][2]) * extents[2];

    // Compute new AABB from center and extents
    out_min[0] = new_center4[0] - new_extents[0];
    out_min[1] = new_center4[1] - new_extents[1];
    out_min[2] = new_center4[2] - new_extents[2];
    out_max[0] = new_center4[0] + new_extents[0];
    out_max[1] = new_center4[1] + new_extents[1];
    out_max[2] = new_center4[2] + new_extents[2];
}

bool frustum_test_aabb_transformed(const Frustum* frustum, vec3 aabb_min, vec3 aabb_max,
                                   mat4 model) {
    // Transform AABB to world space
    vec3 world_min = {0}, world_max = {0};
    aabb_transform(aabb_min, aabb_max, model, world_min, world_max);

    // Test against each frustum plane using the p-vertex method
    // For each plane, find the vertex most in the direction of the plane normal (p-vertex)
    // If p-vertex is behind the plane, the AABB is completely outside
    for (int i = 0; i < 6; i++) {
        const float* plane = frustum->planes[i];

        // Find p-vertex (the corner most in direction of plane normal)
        vec3 p_vertex;
        p_vertex[0] = (plane[0] >= 0.0f) ? world_max[0] : world_min[0];
        p_vertex[1] = (plane[1] >= 0.0f) ? world_max[1] : world_min[1];
        p_vertex[2] = (plane[2] >= 0.0f) ? world_max[2] : world_min[2];

        // Test if p-vertex is behind the plane (negative half-space)
        float distance =
            plane[0] * p_vertex[0] + plane[1] * p_vertex[1] + plane[2] * p_vertex[2] + plane[3];

        if (distance < 0.0f) {
            return false; // AABB is completely outside this plane
        }
    }

    return true; // AABB is inside or intersects the frustum
}
