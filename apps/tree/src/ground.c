#include <math.h>
#include <stdlib.h>

#include "cetra/mesh_builder.h"

#include "ground.h"

// Central-difference step for ground_normal_at. Wide enough that the difference is not
// last-bit noise on a 620-unit dome, narrow enough to resolve the shore's own curvature.
#define GROUND_NORMAL_STEP 0.5f

/*
 * Value noise, deterministic and self-contained (spec 11.44).
 *
 * NOT veg_perlin2, whose permutation table is file-static global state seeded by whoever
 * called veg_noise_seed last. ground_height_at is read by the island mesh, the seabed, the
 * bed bake, the grass scatter, the leaf litter and the rock placement, and a ground whose
 * SHAPE depended on which of those ran first would be a worse failure than the circle this
 * exists to break. A hash has no state to get wrong.
 */
static float ground_hash(int xi, int zi) {
    unsigned int h = (unsigned int)(xi * 374761393) ^ (unsigned int)(zi * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffffu) / (float)0xffffff;
}

static float ground_noise(float x, float z) {
    const float fx = floorf(x), fz = floorf(z);
    const int xi = (int)fx, zi = (int)fz;
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float a = ground_hash(xi, zi), b = ground_hash(xi + 1, zi);
    const float c = ground_hash(xi, zi + 1), d = ground_hash(xi + 1, zi + 1);
    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
}

// See ground.h for what the wobble is for and why its constants live there. Faded out at
// BOTH ends, and the amplitude is bounded well under the crown's rise so the noise can never
// cut the island in two.
static float ground_wobble(float x, float z, float d) {
    const float amp = GROUND_WOBBLE_M * GROUND_UNITS_PER_METRE;
    const float n = ground_noise(x / GROUND_WOBBLE_SPAN, z / GROUND_WOBBLE_SPAN) * 0.68f +
                    ground_noise(x / GROUND_WOBBLE_FINE, z / GROUND_WOBBLE_FINE) * 0.32f;
    // Zero at the centre, full by the time the beach starts. The tree's footing is flat.
    const float t = fminf(d / (GROUND_SHORE_R * 0.55f), 1.0f);
    /*
     * ...and back to zero by the RIM, because the seabed branch beyond it carries no wobble
     * and a term that stops abruptly is a step.
     *
     * Without this ground_height_at jumps by up to the full amplitude across d = GROUND_RADIUS
     * -- a 3.5-unit cliff ringing the island, which the header has claimed since 11.35 that
     * this function does not have. It also makes the rim row a circle at ONE height, which is
     * what lets the island and the seabed meet there whatever ring counts they were built with.
     */
    const float o =
        fminf(2.0f * (GROUND_RADIUS - d) / (GROUND_RADIUS - GROUND_SHORE_R), 1.0f);
    const float oc = fmaxf(o, 0.0f);
    return (n * 2.0f - 1.0f) * amp * (t * t * (3.0f - 2.0f * t)) *
           (oc * oc * (3.0f - 2.0f * oc));
}

float ground_height_at(float x, float z) {
    const float d = sqrtf(x * x + z * z);
    const float shore = GROUND_SHORE_R;
    const float rise = GROUND_CROWN_RISE;
    const float slope = GROUND_BEACH_SLOPE;

    if (d < shore) {
        /*
         * The berm: a cubic Hermite from the crown to the shore, pinned at BOTH ends by
         * value and by slope -- (0, 0) flat, and (shore, -rise) arriving at exactly the
         * beach slope. That last pin is what makes the sand flat where it meets the water
         * instead of steepest there, which is the paraboloid's failing and the whole reason
         * this is not one.
         */
        // Hermite tangents are per unit of t, so the shore's slope scales by the span:
        // dy/dt = (dy/dd) * shore, and dy/dd is NEGATIVE going out. Dropping that sign puts
        // a trough just inside the waterline, which is the one place it would be seen.
        const float t = d / shore;
        const float h01 = t * t * (3.0f - 2.0f * t);
        const float h11 = t * t * (t - 1.0f);
        return h01 * (-rise) + h11 * (-slope * shore) + ground_wobble(x, z, d);
    }
    if (d < GROUND_RADIUS) {
        // The face and the terrace: ONE straight slope, continuing under the water without
        // a crease at the line the eye is most likely to be looking at.
        return -rise - slope * (d - shore) + ground_wobble(x, z, d);
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

float ground_shore_height(void) {
    // The still level, taken on the UNWOBBLED profile at the shore radius. It has to be one
    // number for the whole scene, and the wobble is what makes the waterline wander across
    // it -- pick it off a wobbled sample and the sea would sit at whatever height one
    // arbitrary bearing happened to have.
    return -GROUND_CROWN_RISE;
}

/*
 * The beach's colours, sRGB, in the order the water leaves them.
 *
 * The darkest line on a beach is AT the waterline, and it is lighter on both sides of it --
 * which is not obvious and is most of why a first attempt at this looks like mud.
 *
 * SUBMERGED sand is pale. It is seen through water that scatters light back on the way in
 * and on the way out, so the bed reads brighter and cooler than the same sand in air, and
 * this is the colour the shallows' turquoise is made of: the water tints a pale bed, it does
 * not illuminate a dark one. A submerged colour as dark as the wet strip turns the whole
 * lagoon brown, whatever the absorption is doing.
 *
 * All four are NEAR-WHITE and barely saturated, which is what tropical sand actually is and
 * not what it looks like on a screen: it reads as colour because the light landing on it is
 * coloured. Authored warm and tan, it comes out orange under a low sun and brown under
 * water, which is where this started.
 *
 * WET sand, the strip just above the water, is the dark one: no water film to scatter, and
 * grains still saturated so they refract into each other instead of back at the eye.
 *
 * Then it dries, then it grades into the upland the tree stands on.
 */
static const vec3 GROUND_SAND_SUBMERGED = {0.87f, 0.86f, 0.80f};
static const vec3 GROUND_SAND_WET = {0.66f, 0.62f, 0.55f};
static const vec3 GROUND_SAND_DRY = {0.93f, 0.90f, 0.82f};
static const vec3 GROUND_UPLAND = {0.44f, 0.46f, 0.26f};

// Over how much depth the submerged pale gives way to the waterline's dark, in metres. Short:
// this is the last few centimetres of water, not a gradient across the lagoon.
#define GROUND_SUBMERGED_FADE_M 0.12f

static void ground_smooth_lerp(const vec3 a, const vec3 b, float t, vec3 out) {
    glm_vec3_lerp((float*)a, (float*)b, t * t * (3.0f - 2.0f * t), out);
}

void ground_beach_color(float height_above_water, vec4 out) {
    const float m = height_above_water / GROUND_UNITS_PER_METRE;
    vec3 rgb = GLM_VEC3_ZERO_INIT; // every branch writes it; seeded so the analyser can see it
    if (m <= -GROUND_SUBMERGED_FADE_M) {
        glm_vec3_copy((float*)GROUND_SAND_SUBMERGED, rgb);
    } else if (m <= 0.0f) {
        ground_smooth_lerp(GROUND_SAND_SUBMERGED, GROUND_SAND_WET,
                           (m + GROUND_SUBMERGED_FADE_M) / GROUND_SUBMERGED_FADE_M, rgb);
    } else if (m <= GROUND_WET_SAND_M) {
        glm_vec3_copy((float*)GROUND_SAND_WET, rgb);
    } else if (m <= GROUND_DRY_SAND_M) {
        ground_smooth_lerp(GROUND_SAND_WET, GROUND_SAND_DRY,
                           (m - GROUND_WET_SAND_M) / (GROUND_DRY_SAND_M - GROUND_WET_SAND_M), rgb);
    } else if (m <= GROUND_UPLAND_M) {
        ground_smooth_lerp(GROUND_SAND_DRY, GROUND_UPLAND,
                           (m - GROUND_DRY_SAND_M) / (GROUND_UPLAND_M - GROUND_DRY_SAND_M), rgb);
    } else {
        glm_vec3_copy((float*)GROUND_UPLAND, rgb);
    }
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    out[3] = 1.0f;
}

void ground_normal_at(float x, float z, vec3 out) {
    const float h = GROUND_NORMAL_STEP;
    float dx = ground_height_at(x + h, z) - ground_height_at(x - h, z);
    float dz = ground_height_at(x, z + h) - ground_height_at(x, z - h);
    vec3 n = {-dx, 2.0f * h, -dz};
    glm_vec3_normalize_to(n, out);
}

float ground_sphere_fit(vec3 out_center) {
    /*
     * The berm's curvature at the crown, and a sphere tangent there matching it.
     *
     * Near t = 0 the Hermite is h01 -> 3t^2 and h11 -> -t^2, so with a tangent of
     * -slope*shore the profile opens as -t^2 * (3*rise - slope*shore) and its curvature is
     * twice that over shore^2. Taken from the profile rather than from a remembered
     * paraboloid formula, which is what it was until 11.44 -- and which would have gone
     * quietly wrong the moment the shape did.
     */
    const float shore = GROUND_SHORE_R;
    const float k = 3.0f * GROUND_CROWN_RISE - GROUND_BEACH_SLOPE * shore;
    const float radius = shore * shore / (2.0f * k);
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
    // The beach's colour, which is where the sand-to-upland grade lives -- see ground.h for
    // why it rides vertex colour rather than a second albedo map.
    mesh->colors = malloc(num_vertices * 4 * sizeof(float));
    mesh->index_count = num_triangles * 3;
    mesh->indices = malloc(mesh->index_count * sizeof(unsigned int));

    // Centre vertex (crown). Through ground_height_at like every other row, plus the
    // translation the caller will apply -- see the loop below for why that matters.
    mesh->vertices[0] = 0.0f;
    mesh->vertices[1] = ground_height_at(0.0f, 0.0f) + height;
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
    ground_beach_color(ground_height_at(0.0f, 0.0f) - ground_shore_height(), mesh->colors);

    const float shore = ground_shore_height();
    int vi = 1;
    for (int r = 1; r <= rings; r++) {
        float ring_radius = radius * (float)r / rings;

        for (int s = 0; s < segments; s++) {
            float angle = 2.0f * (float)M_PI * s / segments;
            float x = ring_radius * cosf(angle);
            float z = ring_radius * sinf(angle);

            /*
             * Height and normal from ground_height_at and ground_normal_at, NOT from the
             * profile written out again here.
             *
             * This loop carried its own copy of the paraboloid and its own copy of the
             * paraboloid's slope until spec 11.44, while the header two files up claimed a
             * single authority that the bed, the grass, the seabed and the leaf litter all
             * read. They did; the island mesh did not. Nothing showed it because the two
             * expressions agreed -- and the moment the profile gained a beach shelf they
             * would have stopped agreeing, leaving the island standing at one shape while
             * the water shoaled against another.
             *
             * The +height is the caller's translation, which ground_height_at already
             * includes and this mesh is built without.
             */
            mesh->vertices[vi * 3] = x;
            mesh->vertices[vi * 3 + 1] = ground_height_at(x, z) + height;
            mesh->vertices[vi * 3 + 2] = z;

            vec3 normal = {0.0f, 1.0f, 0.0f}; // out-param; seeded so the analyser can see it
            ground_normal_at(x, z, normal);
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

            ground_beach_color(ground_height_at(x, z) - shore, &mesh->colors[vi * 4]);

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
    // With colours, so the bed carries the same beach banding the island does. Its rim row
    // shares the island's last row exactly, and a seam where one side is coloured sand and
    // the other is not would show as a ring however well the geometry matches.
    if (!mb_init(&mb, vres, ires, true))
        return false;

    const float shore = ground_shore_height();
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
            // approaching one.
            //
            // Straight through, with NO translation added back. ground_height_at already
            // reports world space and this node carries an identity transform -- unlike the
            // island's, which is built about its crown and translated down. Adding
            // GROUND_HEIGHT here to "undo" that translation lifted the whole bed by 190 units,
            // which put its inner ring 47.5 units clear of the waterline: a black wall standing
            // around the island, visible from any camera outside the rim.
            vec3 p = {x, ground_height_at(x, z), z};
            vec3 n = GLM_VEC3_ZERO_INIT;
            ground_normal_at(x, z, n);
            // Tangent along the circle, which is perpendicular to the radial slope for a
            // surface of revolution -- so it is already orthogonal to the normal and needs no
            // Gram-Schmidt. Matches the island mesh's convention, handedness +1.
            vec3 t = {-sa, 0.0f, ca};
            vec4 rgba = GLM_VEC4_ZERO_INIT; // out-param; seeded for the analyser
            ground_beach_color(p[1] - shore, rgba);
            mb_vertex(&mb, p, n, t, (0.5f + 0.5f * x / GROUND_SEABED_RADIUS) * uv_tiles,
                      (0.5f + 0.5f * z / GROUND_SEABED_RADIUS) * uv_tiles, 0.0f, 0.0f, rgba);
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
