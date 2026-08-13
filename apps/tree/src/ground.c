#include <math.h>
#include <stdlib.h>

#include "ground.h"

float ground_height_at(float x, float z) {
    float d = sqrtf(x * x + z * z);
    if (d >= GROUND_RADIUS)
        return -GROUND_HEIGHT;
    float t = d / GROUND_RADIUS;
    return GROUND_HEIGHT * (1.0f - t * t) - GROUND_HEIGHT;
}

float ground_sphere_fit(vec3 out_center) {
    // A paraboloid y = -H*t^2 has curvature 2H/R^2 at its crown, and a sphere tangent there
    // matches it with radius R^2/(2H). With H well under R the two stay together far past
    // the crown: at this island's own numbers they differ by 0.01 units over the canopy and
    // 2 units at the rim, which is nothing a falling leaf can show.
    const float radius = GROUND_RADIUS * GROUND_RADIUS / (2.0f * GROUND_HEIGHT);
    out_center[0] = 0.0f;
    out_center[1] = -radius; // tangent at the crown, which the header puts at y = 0
    out_center[2] = 0.0f;
    return radius;
}

void ground_build_mesh(Mesh* mesh, int rings, int segments, float uv_tiles) {
    const float radius = GROUND_RADIUS;
    const float height = GROUND_HEIGHT;
    // The mesh is built about the CROWN at +height and the caller translates it down, so the
    // arithmetic here is the untranslated profile. ground_height_at includes the translation.
    int num_vertices = 1 + rings * segments; // centre + rings
    int num_triangles = segments + (rings - 1) * segments * 2;

    mesh->vertex_count = num_vertices;
    mesh->vertices = malloc(num_vertices * 3 * sizeof(float));
    mesh->normals = malloc(num_vertices * 3 * sizeof(float));
    mesh->tex_coords = malloc(num_vertices * 2 * sizeof(float));
    mesh->tangents = malloc(num_vertices * 4 * sizeof(float)); // xyz + handedness
    mesh->index_count = num_triangles * 3;
    mesh->indices = malloc(mesh->index_count * sizeof(unsigned int));

    // Centre vertex (top of dome)
    mesh->vertices[0] = 0.0f;
    mesh->vertices[1] = height;
    mesh->vertices[2] = 0.0f;
    mesh->normals[0] = 0.0f;
    mesh->normals[1] = 1.0f;
    mesh->normals[2] = 0.0f;
    mesh->tangents[0] = 1.0f;
    mesh->tangents[1] = 0.0f;
    mesh->tangents[2] = 0.0f;
    mesh->tangents[3] = 1.0f; // cross((0,1,0), (1,0,0)) = (0,0,1)
    mesh->tex_coords[0] = 0.5f * uv_tiles;
    mesh->tex_coords[1] = 0.5f * uv_tiles;

    int vi = 1;
    for (int r = 1; r <= rings; r++) {
        float ring_radius = radius * (float)r / rings;
        float ring_height = height * (1.0f - ((float)r / rings) * ((float)r / rings));

        for (int s = 0; s < segments; s++) {
            float angle = 2.0f * (float)M_PI * s / segments;
            float x = ring_radius * cosf(angle);
            float z = ring_radius * sinf(angle);

            mesh->vertices[vi * 3] = x;
            mesh->vertices[vi * 3 + 1] = ring_height;
            mesh->vertices[vi * 3 + 2] = z;

            // True surface normal of the dome y = height * (1 - (d/radius)^2),
            // whose slope at distance d is 2*height*d/radius^2. The old normal
            // was the radial direction, which tilted the ground up to 60 degrees
            // off vertical -- it faced sideways and never caught the sun.
            float slope = 2.0f * height * ring_radius / (radius * radius);
            vec3 normal = {cosf(angle) * slope, 1.0f, sinf(angle) * slope};
            glm_vec3_normalize(normal);
            mesh->normals[vi * 3] = normal[0];
            mesh->normals[vi * 3 + 1] = normal[1];
            mesh->normals[vi * 3 + 2] = normal[2];

            // Tangent along the circle (perpendicular to radial). The shader
            // derives the bitangent as cross(N, T), which for this normal and
            // tangent works out to (cos, -slope, sin) -- the slope-tilted
            // vector this used to store explicitly -- so the handedness is +1.
            mesh->tangents[vi * 4] = -sinf(angle);
            mesh->tangents[vi * 4 + 1] = 0.0f;
            mesh->tangents[vi * 4 + 2] = cosf(angle);
            mesh->tangents[vi * 4 + 3] = 1.0f;

            mesh->tex_coords[vi * 2] = (0.5f + 0.5f * x / radius) * uv_tiles;
            mesh->tex_coords[vi * 2 + 1] = (0.5f + 0.5f * z / radius) * uv_tiles;

            vi++;
        }
    }

    int ii = 0;

    // Centre fan (first ring). Wound counter-clockwise as seen from above, so
    // the lit side faces the sky -- the original order was reversed, which put
    // every ground triangle's front face underground where back-face culling
    // threw it away.
    for (int s = 0; s < segments; s++) {
        mesh->indices[ii++] = 0;
        mesh->indices[ii++] = 1 + (s + 1) % segments;
        mesh->indices[ii++] = 1 + s;
    }

    for (int r = 1; r < rings; r++) {
        int ring_start = 1 + (r - 1) * segments;
        int next_ring_start = 1 + r * segments;

        for (int s = 0; s < segments; s++) {
            int curr = ring_start + s;
            int next = ring_start + (s + 1) % segments;
            int curr_outer = next_ring_start + s;
            int next_outer = next_ring_start + (s + 1) % segments;

            mesh->indices[ii++] = curr;
            mesh->indices[ii++] = next_outer;
            mesh->indices[ii++] = curr_outer;

            mesh->indices[ii++] = curr;
            mesh->indices[ii++] = next;
            mesh->indices[ii++] = next_outer;
        }
    }

    mesh->draw_mode = MESH_TRIANGLES;
    // Required: the renderer frustum-culls on this. Left at the zero AABB
    // create_mesh starts with, the ground collapses to a point at the origin
    // and gets culled the moment that point leaves the view.
    calculate_aabb(mesh);
}
