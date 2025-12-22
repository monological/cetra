#include <stdlib.h>
#include <math.h>
#include <cglm/cglm.h>

#include "common.h"
#include "ext/log.h"
#include "geometry.h"
#include "mesh.h"
#include "util.h"

/*
 * Bezier curve functions
 */
void cubic_bezier_curve_point(const Curve* curve, float t, vec3 result) {
    // t is the parameter of the curve, ranging from 0 to 1.
    // one_minus_t is the complementary parameter to t.
    float one_minus_t = 1.0f - t;
    float t_squared = t * t;
    float one_minus_t_squared = one_minus_t * one_minus_t;
    float one_minus_t_cubed = one_minus_t_squared * one_minus_t;
    float t_cubed = t_squared * t;

    for (int i = 0; i < 3; i++) { // Loop through x, y, and z coordinates
        // The cubic Bézier curve equation consists of four terms.
        // Each term involves a control point and a polynomial in t.

        // First term: (1-t)^3 * P0
        float first_term = one_minus_t_cubed * curve->control_points[0][i];

        // Second term: 3 * (1-t)^2 * t * P1
        float second_term = 3 * one_minus_t_squared * t * curve->control_points[1][i];

        // Third term: 3 * (1-t) * t^2 * P2
        float third_term = 3 * one_minus_t * t_squared * curve->control_points[2][i];

        // Fourth term: t^3 * P3
        float fourth_term = t_cubed * curve->control_points[3][i];

        // The point on the curve is a weighted sum of these terms.
        result[i] = first_term + second_term + third_term + fourth_term;
    }
}

Curve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity, float line_width) {
    Curve* curve = malloc(sizeof(Curve));
    if (!curve) {
        log_error("Failed to allocate memory for Curve");
        return NULL;
    }

    float direction_x =
        (end[0] - start[0] >= 0) ? 1.0f : -1.0f; // 1 if end is to the right of start, -1 otherwise
    float direction_y =
        (end[1] - start[1] >= 0) ? 1.0f : -1.0f; // 1 if end is above start, -1 otherwise

    float horizontal_offset = fabs(end[0] - start[0]) / 3.0f * intensity;
    float vertical_offset = fabs(end[1] - start[1]) / 3.0f * intensity;

    glm_vec3_copy(start, curve->control_points[0]);
    glm_vec3_copy(end, curve->control_points[3]);

    // Adjust control points based on the directions
    curve->control_points[1][0] = start[0] + horizontal_offset * direction_x;
    curve->control_points[1][1] = start[1] + vertical_offset * direction_y;
    curve->control_points[1][2] = start[2];

    curve->control_points[2][0] = end[0] - horizontal_offset * direction_x;
    curve->control_points[2][1] = end[1] - vertical_offset * direction_y;
    curve->control_points[2][2] = end[2];

    // Adjust vertical positions of control points
    if (direction_y > 0) {
        curve->control_points[1][1] -= vertical_offset;
        curve->control_points[2][1] += vertical_offset;
    } else {
        curve->control_points[1][1] += vertical_offset;
        curve->control_points[2][1] -= vertical_offset;
    }

    curve->line_width = line_width;

    return curve;
}

void free_curve(Curve* curve) {
    if (curve) {
        free(curve);
    }
}

void generate_point_to_mesh(Mesh* mesh, const Point* point) {
    mesh->vertex_count = 1;
    float* new_vertices = (float*)safe_realloc(mesh->vertices, 3 * sizeof(float));
    if (!new_vertices) {
        return;
    }
    mesh->vertices = new_vertices;

    mesh->vertices[0] = point->position[0];
    mesh->vertices[1] = point->position[1];
    mesh->vertices[2] = point->position[2];

    // Points don't use indices
    mesh->index_count = 0;
    free(mesh->indices);
    mesh->indices = NULL;
}

void generate_circle_to_mesh(Mesh* mesh, const Circle* circle) {
    const int segments = NUM_CIRCLE_SEGMENTS; // Number of segments for approximation

    MeshDrawMode draw_mode = circle->filled ? TRIANGLES : LINE_STRIP;
    if (draw_mode == TRIANGLES) {
        // Set up for filled circle
        mesh->vertex_count = segments + 1; // One vertex per segment plus the center
        mesh->index_count = segments * 3;  // Three indices per triangle

        float* new_vertices =
            (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
        unsigned int* new_indices =
            (unsigned int*)safe_realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

        if (!new_vertices || !new_indices) {
            if (new_vertices)
                mesh->vertices = new_vertices;
            if (new_indices)
                mesh->indices = new_indices;
            return;
        }
        mesh->vertices = new_vertices;
        mesh->indices = new_indices;

        // Center vertex
        mesh->vertices[0] = circle->position[0];
        mesh->vertices[1] = circle->position[1];
        mesh->vertices[2] = circle->position[2];

        // Circle vertices
        for (int i = 0; i < segments; ++i) {
            float theta = 2.0f * GLM_PI * i / segments;
            mesh->vertices[(i + 1) * 3] = circle->position[0] + circle->radius * cosf(theta);
            mesh->vertices[(i + 1) * 3 + 1] = circle->position[1] + circle->radius * sinf(theta);
            mesh->vertices[(i + 1) * 3 + 2] = circle->position[2];
        }

        // Indices for triangles
        for (int i = 0; i < segments; ++i) {
            mesh->indices[i * 3] = 0;
            mesh->indices[i * 3 + 1] = i + 1;
            mesh->indices[i * 3 + 2] = (i + 1) % segments + 1;
        }
    } else if (draw_mode == LINE_STRIP) {
        mesh->vertex_count = segments + 1; // One vertex per segment plus one to close the loop
        mesh->index_count = segments + 1;  // One index per vertex

        float* new_vertices =
            (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
        unsigned int* new_indices =
            (unsigned int*)safe_realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

        if (!new_vertices || !new_indices) {
            if (new_vertices)
                mesh->vertices = new_vertices;
            if (new_indices)
                mesh->indices = new_indices;
            return;
        }
        mesh->vertices = new_vertices;
        mesh->indices = new_indices;

        // Circle vertices
        for (int i = 0; i < segments; ++i) {
            float theta = 2.0f * GLM_PI * i / segments;
            mesh->vertices[i * 3] = circle->position[0] + circle->radius * cosf(theta);
            mesh->vertices[i * 3 + 1] = circle->position[1] + circle->radius * sinf(theta);
            mesh->vertices[i * 3 + 2] = circle->position[2];

            mesh->indices[i] = i; // Index for each vertex
        }
        mesh->indices[segments] = 0; // Close the loop
    }

    if (draw_mode == LINE_STRIP) {
        mesh->line_width = circle->line_width;
    }
    mesh->draw_mode = draw_mode;
}

void generate_rect_to_mesh(Mesh* mesh, const Rect* rect) {
    if (!mesh || !rect) {
        return;
    }

    // Calculate half dimensions
    float half_width = rect->size[0] / 2.0f;
    float half_height = rect->size[1] / 2.0f;

    const float corner_radius = rect->corner_radius;
    if (corner_radius > 0.0f) {

        const int resolution = RECT_RESOLUTION; // Resolution of the curves
        const float theta_step =
            (float)GLM_PI / 2 / (resolution - 1); // Quarter-circle for the corner
        const int total_vertices = rect->filled
                                       ? (resolution * 4 + 1)
                                       : (resolution * 4); // +1 for center vertex if filled
        mesh->vertex_count = total_vertices;

        float* new_vertices =
            (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
        if (!new_vertices) {
            return;
        }
        mesh->vertices = new_vertices;

        size_t vertex_index = 0;

        // Define the centered points of the arcs for the four corners
        const vec3 arc_centers[4] = {
            {(rect->position[0] + rect->size[0] - corner_radius) - half_width,
             (rect->position[1] + rect->size[1] - corner_radius) - half_height, 0.0f}, // Top right
            {(rect->position[0] + rect->size[0] - corner_radius) - half_width,
             (rect->position[1] + corner_radius) - half_height, 0.0f}, // Bottom right
            {(rect->position[0] + corner_radius) - half_width,
             (rect->position[1] + corner_radius) - half_height, 0.0f}, // Bottom left
            {(rect->position[0] + corner_radius) - half_width,
             (rect->position[1] + rect->size[1] - corner_radius) - half_height, 0.0f} // Top left
        };

        const float angles[4] = {
            -GLM_PI_2, // Top right starts at -π/2 radians
            0,         // Bottom right starts at 0 radians
            GLM_PI_2,  // Bottom left starts at π/2 radians
            GLM_PI     // Top left starts at π radians
        };

        for (int c = 0; c < 4; c++) {
            // Iterate over each corner arc
            for (int i = 0; i < resolution; i++) {
                float theta =
                    angles[c] + theta_step * i; // Calculate the angle for the current vertex
                mesh->vertices[vertex_index * 3] =
                    arc_centers[c][0] + cosf(theta) * corner_radius; // X coordinate
                mesh->vertices[vertex_index * 3 + 1] =
                    arc_centers[c][1] - sinf(theta) * corner_radius; // Y coordinate
                mesh->vertices[vertex_index * 3 + 2] = 0.0f;         // Z coordinate is always 0
                vertex_index++;
            }
        }

        if (rect->filled) {
            // Add the center point for filled rect at the actual center, not the corner
            vec3 rect_center = {rect->position[0], rect->position[1], 0.0f};
            glm_vec3_copy(rect_center, &mesh->vertices[vertex_index * 3]);
            size_t center_vertex_index = vertex_index;

            // Set indices for filled rect (triangles)
            mesh->index_count = resolution * 4 * 3; // 3 indices per triangle
            unsigned int* new_indices = (unsigned int*)safe_realloc(
                mesh->indices, mesh->index_count * sizeof(unsigned int));
            if (!new_indices) {
                return;
            }
            mesh->indices = new_indices;

            size_t index_index = 0;
            for (int i = 0; i < resolution * 4; i++) {
                mesh->indices[index_index++] = center_vertex_index;
                mesh->indices[index_index++] = (i + 1) % (resolution * 4);
                mesh->indices[index_index++] = i;
            }

            mesh->draw_mode = TRIANGLES;
        } else {
            // Set indices for non-filled rect (line strip)
            mesh->index_count = resolution * 4 + 1; // +1 to close the loop
            unsigned int* new_indices = (unsigned int*)safe_realloc(
                mesh->indices, mesh->index_count * sizeof(unsigned int));
            if (!new_indices) {
                return;
            }
            mesh->indices = new_indices;

            for (size_t i = 0; i < mesh->index_count; ++i) {
                mesh->indices[i] = i % (resolution * 4);
            }

            mesh->line_width = rect->line_width;
            mesh->draw_mode = LINE_STRIP;
        }
    } else {
        // Assuming rect->position is the center of the rect
        vec3 top_left, top_right, bottom_left, bottom_right;

        // Calculate corner positions
        top_left[0] = rect->position[0] - half_width;
        top_left[1] = rect->position[1] + half_height;
        top_left[2] = rect->position[2];

        top_right[0] = rect->position[0] + half_width;
        top_right[1] = rect->position[1] + half_height;
        top_right[2] = rect->position[2];

        bottom_left[0] = rect->position[0] - half_width;
        bottom_left[1] = rect->position[1] - half_height;
        bottom_left[2] = rect->position[2];

        bottom_right[0] = rect->position[0] + half_width;
        bottom_right[1] = rect->position[1] - half_height;
        bottom_right[2] = rect->position[2];

        if (rect->filled) {
            // Handling filled rect with sharp corners
            mesh->vertex_count = 4;
            mesh->index_count = 6; // Two triangles to form a rect

            float* new_vertices =
                (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
            unsigned int* new_indices = (unsigned int*)safe_realloc(
                mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!new_vertices || !new_indices) {
                if (new_vertices)
                    mesh->vertices = new_vertices;
                if (new_indices)
                    mesh->indices = new_indices;
                return;
            }
            mesh->vertices = new_vertices;
            mesh->indices = new_indices;

            glm_vec3_copy(top_left, &mesh->vertices[0]);
            glm_vec3_copy(top_right, &mesh->vertices[3]);
            glm_vec3_copy(bottom_left, &mesh->vertices[6]);
            glm_vec3_copy(bottom_right, &mesh->vertices[9]);

            // Indices - two triangles forming the rect
            unsigned int rect_indices[6] = {0, 2, 1, 1, 2, 3};
            memcpy(mesh->indices, rect_indices, sizeof(rect_indices));

            mesh->draw_mode = TRIANGLES;
        } else {
            mesh->vertex_count = 4;
            mesh->index_count = 5; // Line loop (4 corners + close loop)

            float* new_vertices =
                (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
            unsigned int* new_indices = (unsigned int*)safe_realloc(
                mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!new_vertices || !new_indices) {
                if (new_vertices)
                    mesh->vertices = new_vertices;
                if (new_indices)
                    mesh->indices = new_indices;
                return;
            }
            mesh->vertices = new_vertices;
            mesh->indices = new_indices;

            glm_vec3_copy(top_left, &mesh->vertices[0]);
            glm_vec3_copy(top_right, &mesh->vertices[3]);
            glm_vec3_copy(bottom_right, &mesh->vertices[6]);
            glm_vec3_copy(bottom_left, &mesh->vertices[9]);

            // Indices for line loop
            unsigned int rect_indices[5] = {0, 1, 2, 3, 0};
            memcpy(mesh->indices, rect_indices, sizeof(rect_indices));

            mesh->draw_mode = LINE_LOOP;
            mesh->line_width = rect->line_width;
        }
    }
}

void generate_curve_to_mesh(Mesh* mesh, const Curve* curve) {
    if (!mesh || !curve) {
        return;
    }

    const int resolution = 20; // Number of segments per curve
    mesh->vertex_count = resolution;
    mesh->index_count = (resolution - 1) * 2;

    float* new_vertices =
        (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
    unsigned int* new_indices =
        (unsigned int*)safe_realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

    if (!new_vertices || !new_indices) {
        if (new_vertices)
            mesh->vertices = new_vertices;
        if (new_indices)
            mesh->indices = new_indices;
        return;
    }
    mesh->vertices = new_vertices;
    mesh->indices = new_indices;

    size_t vertex_index = 0, index_index = 0;
    for (int j = 0; j < resolution; ++j) {
        float t = (float)j / (float)(resolution - 1);
        vec3 point = {0};
        cubic_bezier_curve_point(curve, t, point);

        mesh->vertices[vertex_index * 3] = point[0];
        mesh->vertices[vertex_index * 3 + 1] = point[1];
        mesh->vertices[vertex_index * 3 + 2] = point[2];

        if (j < resolution - 1) {
            mesh->indices[index_index++] = vertex_index;
            mesh->indices[index_index++] = vertex_index + 1;
        }

        vertex_index++;
    }

    mesh->line_width = curve->line_width;
    mesh->draw_mode = LINE_STRIP;
}

void generate_cylinder_to_mesh(Mesh* mesh, const Cylinder* cylinder) {
    if (!mesh || !cylinder || cylinder->segments < 3) {
        return;
    }

    int segments = cylinder->segments;
    float base_r = cylinder->base_radius;
    float top_r = cylinder->top_radius;
    float h = cylinder->height;

    // Vertices: 2 rings (bottom + top)
    mesh->vertex_count = segments * 2;
    mesh->index_count = segments * 6; // 2 triangles per quad, 3 indices each

    float* new_vertices =
        (float*)safe_realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
    float* new_normals =
        (float*)safe_realloc(mesh->normals, mesh->vertex_count * 3 * sizeof(float));
    float* new_tex_coords =
        (float*)safe_realloc(mesh->tex_coords, mesh->vertex_count * 2 * sizeof(float));
    unsigned int* new_indices =
        (unsigned int*)safe_realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

    if (!new_vertices || !new_normals || !new_tex_coords || !new_indices) {
        if (new_vertices)
            mesh->vertices = new_vertices;
        if (new_normals)
            mesh->normals = new_normals;
        if (new_tex_coords)
            mesh->tex_coords = new_tex_coords;
        if (new_indices)
            mesh->indices = new_indices;
        return;
    }
    mesh->vertices = new_vertices;
    mesh->normals = new_normals;
    mesh->tex_coords = new_tex_coords;
    mesh->indices = new_indices;

    // Generate vertices for bottom and top rings
    for (int i = 0; i < segments; i++) {
        float theta = 2.0f * GLM_PI * i / segments;
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);
        float u = (float)i / segments;

        // Bottom ring vertex (index i)
        mesh->vertices[i * 3 + 0] = cylinder->position[0] + base_r * cos_t;
        mesh->vertices[i * 3 + 1] = cylinder->position[1];
        mesh->vertices[i * 3 + 2] = cylinder->position[2] + base_r * sin_t;

        // Top ring vertex (index segments + i)
        int top_idx = segments + i;
        mesh->vertices[top_idx * 3 + 0] = cylinder->position[0] + top_r * cos_t;
        mesh->vertices[top_idx * 3 + 1] = cylinder->position[1] + h;
        mesh->vertices[top_idx * 3 + 2] = cylinder->position[2] + top_r * sin_t;

        // Calculate normal (pointing outward radially)
        // For a tapered cylinder, the normal tilts based on the slope
        float slope = (base_r - top_r) / h;
        float nx = cos_t;
        float ny = slope;
        float nz = sin_t;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 0.0001f) {
            nx /= len;
            ny /= len;
            nz /= len;
        }

        // Bottom normal
        mesh->normals[i * 3 + 0] = nx;
        mesh->normals[i * 3 + 1] = ny;
        mesh->normals[i * 3 + 2] = nz;

        // Top normal (same direction)
        mesh->normals[top_idx * 3 + 0] = nx;
        mesh->normals[top_idx * 3 + 1] = ny;
        mesh->normals[top_idx * 3 + 2] = nz;

        // UV coordinates: U wraps around, V goes 0 at bottom to 1 at top
        mesh->tex_coords[i * 2 + 0] = u;
        mesh->tex_coords[i * 2 + 1] = 0.0f;

        mesh->tex_coords[top_idx * 2 + 0] = u;
        mesh->tex_coords[top_idx * 2 + 1] = 1.0f;
    }

    // Generate indices for triangles
    size_t idx = 0;
    for (int i = 0; i < segments; i++) {
        int next = (i + 1) % segments;

        // Bottom-left, bottom-right, top-left triangle
        mesh->indices[idx++] = i;
        mesh->indices[idx++] = next;
        mesh->indices[idx++] = segments + i;

        // Top-left, bottom-right, top-right triangle
        mesh->indices[idx++] = segments + i;
        mesh->indices[idx++] = next;
        mesh->indices[idx++] = segments + next;
    }

    mesh->draw_mode = TRIANGLES;
}
