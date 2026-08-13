#include <math.h>
#include <stdlib.h>

#include "cetra/mesh_builder.h"

#include "ground.h"

// Central-difference step for ground_normal_at. Wide enough that the difference is not
// last-bit noise on a 620-unit dome, narrow enough to resolve the shore's own curvature.
#define GROUND_NORMAL_STEP 0.5f

float ground_height_at(float x, float z) {
    float d = sqrtf(x * x + z * z);
    if (d < GROUND_RADIUS) {
        float t = d / GROUND_RADIUS;
        return GROUND_HEIGHT * (1.0f - t * t) - GROUND_HEIGHT;
    }
    /*
     * The seabed, past the rim.
     *
     * Continues the rim's own descent and flattens into it, rather than stepping to a
     * constant: `1 - (1 - u)^2` over u = (d - R)/(seabedR - R) starts at the dome's depth with
     * slope 2*DROP/span and arrives level at the outer edge. GROUND_SEABED_DROP is derived so
     * that starting slope equals the dome's arriving one -- see the header -- so the flank the
     * shore stands on and the open bed are one surface with no kink between them.
     */
    float u = fminf((d - GROUND_RADIUS) / (GROUND_SEABED_RADIUS - GROUND_RADIUS), 1.0f);
    float ease = 1.0f - (1.0f - u) * (1.0f - u);
    return -GROUND_HEIGHT - GROUND_SEABED_DROP * ease;
}

void ground_normal_at(float x, float z, vec3 out) {
    const float h = GROUND_NORMAL_STEP;
    float dx = ground_height_at(x + h, z) - ground_height_at(x - h, z);
    float dz = ground_height_at(x, z + h) - ground_height_at(x, z - h);
    vec3 n = {-dx, 2.0f * h, -dz};
    glm_vec3_normalize_to(n, out);
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

bool ground_build_seabed(Mesh* mesh, int rings, int segments, float uv_tiles) {
    MeshBuilder mb;
    // Exact counts, so the builder never copies: (rings + 1) rows of `segments` vertices,
    // and two triangles per quad between consecutive rows.
    const size_t vres = (size_t)(rings + 1) * (size_t)segments;
    const size_t ires = (size_t)rings * (size_t)segments * 6;
    if (!mb_init(&mb, vres, ires, false))
        return false;

    for (int r = 0; r <= rings; r++) {
        // Geometric in the RADIUS, so the ring spacing grows with distance -- see the header.
        const float u = (float)r / (float)rings;
        const float radius =
            GROUND_RADIUS * powf(GROUND_SEABED_RADIUS / GROUND_RADIUS, u);
        for (int s = 0; s < segments; s++) {
            const float angle = 2.0f * (float)M_PI * (float)s / (float)segments;
            const float ca = cosf(angle), sa = sinf(angle);
            const float x = radius * ca;
            const float z = radius * sa;
            // Height and normal both from ground_height_at, so the rim row lands exactly on
            // the island's own last row and the two meshes share an edge rather than
            // approaching one. The island is built about its crown and translated down by
            // GROUND_HEIGHT; this mesh is authored in world space, so the translation comes
            // back out here.
            vec3 p = {x, ground_height_at(x, z) + GROUND_HEIGHT, z};
            vec3 n = GLM_VEC3_ZERO_INIT;
            ground_normal_at(x, z, n);
            // Tangent along the circle, which is perpendicular to the radial slope for a
            // surface of revolution -- so it is already orthogonal to the normal and needs no
            // Gram-Schmidt. Matches the island mesh's convention, handedness +1.
            vec3 t = {-sa, 0.0f, ca};
            mb_vertex(&mb, p, n, t, (0.5f + 0.5f * x / GROUND_SEABED_RADIUS) * uv_tiles,
                      (0.5f + 0.5f * z / GROUND_SEABED_RADIUS) * uv_tiles, 0.0f, 0.0f, NULL);
        }
    }

    for (int r = 0; r < rings; r++) {
        const unsigned int inner = (unsigned int)(r * segments);
        const unsigned int outer = (unsigned int)((r + 1) * segments);
        for (int s = 0; s < segments; s++) {
            const unsigned int s1 = (unsigned int)((s + 1) % segments);
            // Same winding as the island's rings: counter-clockwise seen from above, so the
            // lit face points at the sky rather than into the bed.
            mb_tri(&mb, inner + (unsigned int)s, outer + s1, outer + (unsigned int)s);
            mb_tri(&mb, inner + (unsigned int)s, inner + s1, outer + s1);
        }
    }

    mesh->draw_mode = MESH_TRIANGLES;
    return mb_transfer(&mb, mesh);
}
