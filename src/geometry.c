#include <stdlib.h>
#include <math.h>
#include <cglm/cglm.h>

#include "geometry.h"
#include "mesh.h"
#include "common.h"
#include "util.h"


/*
 * Bezier curve functions
 */
void cubic_bezier_curve_point(const CubicBezierCurve* curve, float t, vec3 result) {
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

CubicBezierCurve* generate_s_shaped_bezier_curve(vec3 start, vec3 end, float intensity, 
        float line_width) {
    CubicBezierCurve* curve = malloc(sizeof(CubicBezierCurve));
    if (!curve) {
        fprintf(stderr, "Failed to allocate memory for CubicBezierCurve\n");
        return NULL;
    }

    float direction_x = (end[0] - start[0] >= 0) ? 1.0f : -1.0f; // 1 if end is to the right of start, -1 otherwise
    float direction_y = (end[1] - start[1] >= 0) ? 1.0f : -1.0f; // 1 if end is above start, -1 otherwise

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

void free_cubic_bezier_curve(CubicBezierCurve* curve) {
    if (curve) {
        free(curve);
    }
}

void rasterize_point_to_mesh(Mesh* mesh, const Point* point) {
    mesh->vertex_count = 1;
    mesh->vertices = (float*)realloc(mesh->vertices, 3 * sizeof(float)); // 3 for x, y, z

    if (!mesh->vertices) {
        fprintf(stderr, "Failed to allocate memory for vertices\n");
        return;
    }

    mesh->vertices[0] = point->position[0];
    mesh->vertices[1] = point->position[1];
    mesh->vertices[2] = point->position[2];

    // Points don't use indices
    mesh->index_count = 0;
    free(mesh->indices);
    mesh->indices = NULL;
}

void rasterize_circle_to_mesh(Mesh* mesh, const Circle* circle) {
    const int segments = NUM_CIRCLE_SEGMENTS; // Number of segments for approximation

    MeshDrawMode draw_mode = circle->filled ? TRIANGLES : LINE_STRIP;
    if (draw_mode == TRIANGLES) {
        // Set up for filled circle
        mesh->vertex_count = segments + 1; // One vertex per segment plus the center
        mesh->index_count = segments * 3; // Three indices per triangle

        mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
        mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

        if (!mesh->vertices || !mesh->indices) {
            fprintf(stderr, "Failed to allocate memory for filled circle mesh\n");
            return;
        }

        // Center vertex
        mesh->vertices[0] = circle->position[0];
        mesh->vertices[1] = circle->position[1];
        mesh->vertices[2] = circle->position[2];

        // Circle vertices
        for (int i = 0; i < segments; ++i) {
            float theta = 2.0f * M_PI * i / segments;
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
        // Set up for circle outline
        mesh->vertex_count = segments + 1; // One vertex per segment plus one to close the loop
        mesh->index_count = segments + 1;  // One index per vertex

        mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
        mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

        if (!mesh->vertices || !mesh->indices) {
            fprintf(stderr, "Failed to allocate memory for circle outline mesh\n");
            return;
        }

        // Circle vertices
        for (int i = 0; i < segments; ++i) {
            float theta = 2.0f * M_PI * i / segments;
            mesh->vertices[i * 3] = circle->position[0] + circle->radius * cosf(theta);
            mesh->vertices[i * 3 + 1] = circle->position[1] + circle->radius * sinf(theta);
            mesh->vertices[i * 3 + 2] = circle->position[2];

            mesh->indices[i] = i; // Index for each vertex
        }
        mesh->indices[segments] = 0; // Close the loop
    }

    if(draw_mode == LINE_STRIP) {
        mesh->line_width = circle->line_width;
    }
    mesh->draw_mode = draw_mode;
}


void rasterize_rectangle_to_mesh(Mesh* mesh, const Rectangle* rectangle) {
    if (!mesh || !rectangle) {
        return;
    }

    // Calculate half dimensions
    float half_width = rectangle->size[0] / 2.0f;
    float half_height = rectangle->size[1] / 2.0f;

    const float corner_radius = rectangle->corner_radius;
    if (corner_radius > 0.0f) {

        const int resolution = RECTANGLE_RESOLUTION; // Resolution of the curves
        const float theta_step = (float)M_PI / 2 / (resolution - 1); // Quarter-circle for the corner
        const int total_vertices = rectangle->filled ? (resolution * 4 + 1) : (resolution * 4); // +1 for center vertex if filled
        mesh->vertex_count = total_vertices;

        mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));

        if (!mesh->vertices) {
            fprintf(stderr, "Failed to allocate memory for rounded rectangle mesh\n");
            return;
        }

        size_t vertex_index = 0;

        // Define the centered points of the arcs for the four corners
        vec3 arc_centers[4] = {
            {(rectangle->position[0] + rectangle->size[0] - corner_radius)-half_width, (rectangle->position[1] + rectangle->size[1] - corner_radius)-half_height, 0.0f}, // Top right
            {(rectangle->position[0] + rectangle->size[0] - corner_radius)-half_width, (rectangle->position[1] + corner_radius)-half_height, 0.0f}, // Bottom right
            {(rectangle->position[0] + corner_radius)-half_width, (rectangle->position[1] + corner_radius)-half_height, 0.0f}, // Bottom left
            {(rectangle->position[0] + corner_radius)-half_width, (rectangle->position[1] + rectangle->size[1] - corner_radius)-half_height, 0.0f} // Top left
        };

        float angles[4] = {
            -M_PI_2,      // Top right starts at -π/2 radians
            0,            // Bottom right starts at 0 radians
            M_PI_2,       // Bottom left starts at π/2 radians
            M_PI          // Top left starts at π radians
        };

        for (int c = 0; c < 4; c++) {
            // Iterate over each corner arc
            for (int i = 0; i < resolution; i++) {
                float theta = angles[c] + theta_step * i;  // Calculate the angle for the current vertex
                mesh->vertices[vertex_index * 3] = arc_centers[c][0] + cosf(theta) * corner_radius;      // X coordinate
                mesh->vertices[vertex_index * 3 + 1] = arc_centers[c][1] - sinf(theta) * corner_radius;  // Y coordinate
                mesh->vertices[vertex_index * 3 + 2] = 0.0f; // Z coordinate is always 0
                vertex_index++;
            }
        }

        if (rectangle->filled) {
            // Add the center point for filled rectangle at the actual center, not the corner
            vec3 rectangle_center = {
                rectangle->position[0],
                rectangle->position[1],
                0.0f
            };
            glm_vec3_copy(rectangle_center, &mesh->vertices[vertex_index * 3]);
            size_t center_vertex_index = vertex_index;
            vertex_index++;

            // Set indices for filled rectangle (triangles)
            mesh->index_count = resolution * 4 * 3; // 3 indices per triangle
            mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!mesh->indices) {
                fprintf(stderr, "Failed to allocate memory for indices\n");
                return;
            }

            size_t index_index = 0;
            for (int i = 0; i < resolution * 4; i++) {
                mesh->indices[index_index++] = center_vertex_index;
                mesh->indices[index_index++] = (i + 1) % (resolution * 4);
                mesh->indices[index_index++] = i;
            }


            mesh->draw_mode = TRIANGLES;
        } else {
            // Set indices for non-filled rectangle (line strip)
            mesh->index_count = resolution * 4 + 1; // +1 to close the loop
            mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!mesh->indices) {
                fprintf(stderr, "Failed to allocate memory for indices\n");
                return;
            }

            for (size_t i = 0; i < mesh->index_count; ++i) {
                mesh->indices[i] = i % (resolution * 4);
            }

            mesh->line_width = rectangle->line_width;
            mesh->draw_mode = LINE_STRIP;
        }
    } else {
        // Assuming rectangle->position is the center of the rectangle
        vec3 top_left, top_right, bottom_left, bottom_right;

        // Calculate corner positions
        top_left[0] = rectangle->position[0] - half_width;
        top_left[1] = rectangle->position[1] + half_height;
        top_left[2] = rectangle->position[2];

        top_right[0] = rectangle->position[0] + half_width;
        top_right[1] = rectangle->position[1] + half_height;
        top_right[2] = rectangle->position[2];

        bottom_left[0] = rectangle->position[0] - half_width;
        bottom_left[1] = rectangle->position[1] - half_height;
        bottom_left[2] = rectangle->position[2];

        bottom_right[0] = rectangle->position[0] + half_width;
        bottom_right[1] = rectangle->position[1] - half_height;
        bottom_right[2] = rectangle->position[2];

        if (rectangle->filled) {
            // Handling filled rectangle with sharp corners
            mesh->vertex_count = 4;
            mesh->index_count = 6; // Two triangles to form a rectangle

            mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
            mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!mesh->vertices || !mesh->indices) {
                fprintf(stderr, "Failed to allocate memory for filled rectangle mesh\n");
                return;
            }

            glm_vec3_copy(top_left, &mesh->vertices[0]); // Top left
            glm_vec3_copy(top_right, &mesh->vertices[3]); // Top right
            glm_vec3_copy(bottom_left, &mesh->vertices[6]); // Bottom left
            glm_vec3_copy(bottom_right, &mesh->vertices[9]); // Bottom right

            // Indices - two triangles forming the rectangle
            unsigned int rect_indices[6] = {0, 2, 1, 1, 2, 3};
            memcpy(mesh->indices, rect_indices, sizeof(rect_indices));

            mesh->draw_mode = TRIANGLES;
        } else {
            mesh->vertex_count = 4;
            mesh->index_count = 5; // Line loop (4 corners + close loop)

            mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
            mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

            if (!mesh->vertices || !mesh->indices) {
                fprintf(stderr, "Failed to allocate memory for rectangle outline mesh\n");
                return;
            }

            glm_vec3_copy(top_left, &mesh->vertices[0]); // Top left
            glm_vec3_copy(top_right, &mesh->vertices[3]); // Top right
            glm_vec3_copy(bottom_right, &mesh->vertices[6]); // Bottom right
            glm_vec3_copy(bottom_left, &mesh->vertices[9]); // Bottom left

            // Indices for line loop
            unsigned int outline_indices[5] = {0, 1, 2, 3, 0};
            memcpy(mesh->indices, outline_indices, sizeof(outline_indices));

            mesh->draw_mode = LINE_LOOP;
            mesh->line_width = rectangle->line_width;
        }


    }
}

void rasterize_bezier_curve_to_mesh(Mesh* mesh, CubicBezierCurve* curve) {
    if (!mesh || !curve) {
        return;
    }

    const int resolution = 20; // Number of segments per curve
    mesh->vertex_count = resolution;
    mesh->index_count = (resolution - 1) * 2;

    mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
    mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

    if (!mesh->vertices || !mesh->indices) {
        fprintf(stderr, "Failed to allocate memory for mesh\n");
        return;
    }

    size_t vertex_index = 0, index_index = 0;
    for (int j = 0; j < resolution; ++j) {
        float t = (float)j / (float)(resolution - 1);
        vec3 point;
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


float noise(float x, float y) {
    return sinf(x * 12.9898f) * cosf(y * 78.233f);
}

// Linear interpolation
float lerp(float a, float b, float t) {
    return a + t * (b - a);
}


float fbm_2d(float x, float y, void* params) {

    fbmParams* fbm_params = (fbmParams*)params;

    float total = 0.0f;
    float frequency = 1.0f;
    float amplitude = 1.0f;
    float max_value = 0.0f;  // Used for normalizing result

    for(int i = 0; i < fbm_params->octaves; i++) {
        total += noise(x * frequency, y * frequency) * amplitude;

        max_value += amplitude;
        amplitude *= fbm_params->persistence;
        frequency *= 2.0f;
    }

    return total / max_value;
}

void rasterize_contours_to_mesh(Mesh* mesh, SurfaceFunction surface, void* params, 
        const Rect* bounds, int resolution,
        int levels) {
    if (!mesh || !surface || !bounds) {
        fprintf(stderr, "Invalid parameters for rasterizing contours to mesh.\n");
        return;
    }

    // Allocate vertices and indices
    mesh->vertex_count = resolution * resolution;
    mesh->index_count = (resolution - 1) * (resolution - 1) * 6; // 6 indices per quad (2 triangles)

    mesh->vertices = (float*)realloc(mesh->vertices, mesh->vertex_count * 3 * sizeof(float));
    mesh->indices = (unsigned int*)realloc(mesh->indices, mesh->index_count * sizeof(unsigned int));

    if (!mesh->vertices || !mesh->indices) {
        fprintf(stderr, "Failed to allocate memory for contour mesh\n");
        return;
    }

    // Generate vertices
    float x_step = (bounds->width) / (resolution - 1);
    float y_step = (bounds->height) / (resolution - 1);

    for (int y = 0; y < resolution; ++y) {
        for (int x = 0; x < resolution; ++x) {
            float surface_x = bounds->x + x * x_step;
            float surface_y = bounds->y + y * y_step;
            float height = surface(surface_x, surface_y, params);

            int vertex_index = (y * resolution + x) * 3;
            mesh->vertices[vertex_index] = surface_x;
            mesh->vertices[vertex_index + 1] = surface_y;
            mesh->vertices[vertex_index + 2] = height;
        }
    }

    // Generate indices
    size_t index = 0;
    for (int y = 0; y < resolution - 1; ++y) {
        for (int x = 0; x < resolution - 1; ++x) {
            int base = y * resolution + x;
            // First triangle
            mesh->indices[index++] = base;
            mesh->indices[index++] = base + 1;
            mesh->indices[index++] = base + resolution + 1;

            // Second triangle
            mesh->indices[index++] = base;
            mesh->indices[index++] = base + resolution + 1;
            mesh->indices[index++] = base + resolution;
        }
    }

    mesh->line_width = 0.3f;
    mesh->draw_mode = LINES;
}

