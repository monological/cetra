#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rock.h"

#include "../ext/log.h"
#include "../mesh_builder.h"
#include "../noise.h"
#include "../util.h"

#define ROCK_MAX_SUBDIV     5
#define ROCK_NOISE_PERIOD   256
#define ROCK_NOISE_OCTAVES  4

RockParams rock_default_params(void) {
    RockParams p;
    p.radius = 1.0f;
    p.subdivisions = 3;
    p.roughness = 0.32f;
    p.noise_freq = 1.7f;
    p.seed = 1u;
    return p;
}

// Midpoint cache. Subdivision splits every edge once, and both faces sharing
// that edge must get the SAME new vertex -- otherwise the surface comes apart
// into 20 disconnected patches whose borders meshoptimizer would then refuse to
// touch, which is the whole property this shape was chosen for.
typedef struct EdgeEntry {
    uint64_t key; // (min index << 32) | max index, +1 so zero means empty
    unsigned val;
} EdgeEntry;

typedef struct EdgeMap {
    EdgeEntry* slots;
    size_t mask;
} EdgeMap;

static bool edge_map_init(EdgeMap* m, size_t capacity) {
    size_t n = 16;
    while (n < capacity * 2u)
        n *= 2u;
    m->slots = calloc(n, sizeof(EdgeEntry));
    m->mask = n - 1u;
    return m->slots != NULL;
}

static void edge_map_free(EdgeMap* m) {
    free(m->slots);
    m->slots = NULL;
}

static uint64_t edge_key(unsigned a, unsigned b) {
    unsigned lo = a < b ? a : b;
    unsigned hi = a < b ? b : a;
    return (((uint64_t)lo << 32) | (uint64_t)hi) + 1u;
}

// Open addressing with linear probing. Returns true when the edge was already
// present, writing its vertex index to `out`.
static bool edge_map_find_or_add(EdgeMap* m, uint64_t key, unsigned candidate, unsigned* out) {
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & m->mask;
    for (;;) {
        if (m->slots[i].key == 0u) {
            m->slots[i].key = key;
            m->slots[i].val = candidate;
            *out = candidate;
            return false;
        }
        if (m->slots[i].key == key) {
            *out = m->slots[i].val;
            return true;
        }
        i = (i + 1u) & m->mask;
    }
}

static float rock_fbm(const NoisePerm* t, const vec3 dir, float freq) {
    // Offset off the origin: the lattice is indexed from zero and a unit sphere
    // straddles it, so sampling raw would fold opposite sides of the rock onto
    // each other.
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = freq;
    for (int i = 0; i < ROCK_NOISE_OCTAVES; ++i) {
        float n = noise_perlin3_tiled(t, (dir[0] + 4.0f) * f, (dir[1] + 4.0f) * f,
                                      (dir[2] + 4.0f) * f, ROCK_NOISE_PERIOD);
        sum += amp * n;
        norm += amp;
        amp *= 0.5f;
        f *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

bool rock_build_mesh(const RockParams* p, Mesh* mesh) {
    if (!p || !mesh || p->radius <= 0.0f)
        return false;

    int subdiv = p->subdivisions;
    if (subdiv < 0)
        subdiv = 0;
    if (subdiv > ROCK_MAX_SUBDIV)
        subdiv = ROCK_MAX_SUBDIV;

    // Icosahedron, from the golden ratio. Every vertex has degree five, every
    // face is congruent, and there is no pole and no seam.
    const float t = 1.61803398875f;
    static const int BASE_FACES[20][3] = {
        {0, 11, 5}, {0, 5, 1},   {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
        {1, 5, 9},  {5, 11, 4},  {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4},  {3, 4, 2},   {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
        {4, 9, 5},  {2, 4, 11},  {6, 2, 10},  {8, 6, 7},  {9, 8, 1},
    };
    const float BASE_POS[12][3] = {
        {-1, t, 0}, {1, t, 0},  {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1},  {-t, 0, -1}, {-t, 0, 1},
    };

    // Euler exactly, not an estimate: F = 20*4^n and V = 2 + 10*4^n for a
    // subdivided icosahedron, so both arrays are allocated once at final size.
    size_t pow4 = 1u;
    for (int i = 0; i < subdiv; ++i)
        pow4 *= 4u;
    size_t face_count = 20u * pow4;
    size_t vert_cap = 2u + 10u * pow4;

    vec3* pos = malloc(vert_cap * sizeof(vec3));
    unsigned int* faces = malloc(face_count * 3u * sizeof(unsigned int));
    if (!pos || !faces) {
        log_error("Rock: could not allocate %zu verts / %zu faces", vert_cap, face_count);
        free(pos);
        free(faces);
        return false;
    }

    size_t vcount = 12u;
    for (size_t i = 0; i < 12u; ++i)
        glm_vec3_copy((float*)BASE_POS[i], pos[i]);

    size_t fcount = 20u;
    for (size_t i = 0; i < 20u; ++i) {
        faces[i * 3u + 0u] = (unsigned int)BASE_FACES[i][0];
        faces[i * 3u + 1u] = (unsigned int)BASE_FACES[i][1];
        faces[i * 3u + 2u] = (unsigned int)BASE_FACES[i][2];
    }

    unsigned int* next = NULL;
    for (int level = 0; level < subdiv; ++level) {
        size_t out_faces = fcount * 4u;
        next = malloc(out_faces * 3u * sizeof(unsigned int));
        EdgeMap map;
        if (!next || !edge_map_init(&map, fcount * 3u)) {
            log_error("Rock: subdivision allocation failed at level %d", level);
            free(next);
            free(pos);
            free(faces);
            return false;
        }

        size_t w = 0;
        for (size_t f = 0; f < fcount; ++f) {
            unsigned int a = faces[f * 3u + 0u];
            unsigned int b = faces[f * 3u + 1u];
            unsigned int c = faces[f * 3u + 2u];
            unsigned int mid[3];
            unsigned int pair[3][2] = {{a, b}, {b, c}, {c, a}};
            for (int e = 0; e < 3; ++e) {
                unsigned int u = pair[e][0], v = pair[e][1];
                unsigned int found;
                if (!edge_map_find_or_add(&map, edge_key(u, v), (unsigned int)vcount, &found)) {
                    vec3 m;
                    glm_vec3_add(pos[u], pos[v], m);
                    glm_vec3_scale(m, 0.5f, m);
                    glm_vec3_copy(m, pos[vcount]);
                    vcount++;
                }
                mid[e] = found;
            }
            unsigned int tri[4][3] = {
                {a, mid[0], mid[2]}, {b, mid[1], mid[0]}, {c, mid[2], mid[1]}, {mid[0], mid[1], mid[2]},
            };
            for (int k = 0; k < 4; ++k) {
                next[w * 3u + 0u] = tri[k][0];
                next[w * 3u + 1u] = tri[k][1];
                next[w * 3u + 2u] = tri[k][2];
                w++;
            }
        }
        edge_map_free(&map);
        free(faces);
        faces = next;
        next = NULL;
        fcount = out_faces;
    }

    // Project to the sphere, then displace radially. Done after all subdivision
    // so every level sees the same surface rather than compounding.
    NoisePerm perm;
    noise_perm_init(&perm, p->seed);
    for (size_t i = 0; i < vcount; ++i) {
        vec3 dir;
        glm_vec3_normalize_to(pos[i], dir);
        float d = rock_fbm(&perm, dir, p->noise_freq);
        float r = p->radius * (1.0f + p->roughness * d);
        glm_vec3_scale(dir, r, pos[i]);
    }

    // Normals from the displaced surface. Accumulating face normals is what
    // makes the lumps read as lumps; the sphere's own normal would light it as
    // though it were still smooth.
    vec3* nrm = calloc(vcount, sizeof(vec3));
    if (!nrm) {
        free(pos);
        free(faces);
        return false;
    }
    for (size_t f = 0; f < fcount; ++f) {
        unsigned int a = faces[f * 3u + 0u], b = faces[f * 3u + 1u], c = faces[f * 3u + 2u];
        vec3 e1, e2, fn;
        glm_vec3_sub(pos[b], pos[a], e1);
        glm_vec3_sub(pos[c], pos[a], e2);
        glm_vec3_cross(e1, e2, fn);
        glm_vec3_add(nrm[a], fn, nrm[a]);
        glm_vec3_add(nrm[b], fn, nrm[b]);
        glm_vec3_add(nrm[c], fn, nrm[c]);
    }

    MeshBuilder mb;
    if (!mb_init(&mb, vcount, fcount * 3u, false)) {
        free(pos);
        free(faces);
        free(nrm);
        return false;
    }

    for (size_t i = 0; i < vcount; ++i) {
        vec3 n;
        if (glm_vec3_norm(nrm[i]) > 1e-8f)
            glm_vec3_normalize_to(nrm[i], n);
        else
            glm_vec3_normalize_to(pos[i], n);

        // Spherical UV. The seam is unavoidable in a mapping like this and is
        // left in the UVs alone -- it never splits a vertex, so the geometry
        // stays a closed manifold and the simplifier stays unconstrained.
        vec3 dir;
        glm_vec3_normalize_to(pos[i], dir);
        float u = 0.5f + atan2f(dir[2], dir[0]) / (2.0f * GLM_PIf);
        float v = 0.5f - asinf(dir[1] < -1.0f ? -1.0f : (dir[1] > 1.0f ? 1.0f : dir[1])) / GLM_PIf;

        vec3 tangent = {0.0f, 1.0f, 0.0f};
        glm_vec3_muladds(n, -glm_vec3_dot(n, tangent), tangent);
        if (glm_vec3_norm(tangent) < 1e-6f)
            glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, tangent);
        glm_vec3_normalize(tangent);

        mb_vertex(&mb, pos[i], n, tangent, u, v, 0.0f, 0.0f, NULL);
    }
    for (size_t f = 0; f < fcount; ++f)
        mb_tri(&mb, faces[f * 3u + 0u], faces[f * 3u + 1u], faces[f * 3u + 2u]);

    free(pos);
    free(faces);
    free(nrm);
    return mb_transfer(&mb, mesh);
}
