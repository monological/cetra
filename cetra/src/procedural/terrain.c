#include <math.h>

#include "terrain.h"

#include "../mesh_builder.h"
#include "../noise.h"

// The tiling lattice period noise_perlin3_tiled works over. Coordinates are
// offset to start at zero and the default frequencies keep the finest octave
// well inside one period across the whole terrain, so the tiling never shows.
#define TERRAIN_NOISE_PERIOD 256

// Surface tints, blended per vertex. There is no splat-map system, so this is
// what stops a kilometre of ground reading as one flat hue.
static const vec3 TINT_GRASS = {0.19f, 0.31f, 0.13f};
static const vec3 TINT_DIRT = {0.31f, 0.25f, 0.17f};
static const vec3 TINT_ROCK = {0.42f, 0.40f, 0.38f};
static const vec3 TINT_SNOW = {0.86f, 0.88f, 0.92f};

TerrainParams terrain_default_params(void) {
    TerrainParams p;
    p.extent = 500.0f;
    p.height = 55.0f;
    // Chosen together: the finest octave is base_freq * lacunarity^(octaves-1) =
    // 0.064, whose lattice repeats every 256/0.064 = 4000 units -- four times the
    // terrain's own span, so no octave can tile within view.
    p.base_freq = 0.004f;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.octaves = 5;
    p.seed = 1337u;
    p.tiles = 8;
    p.tile_segments = 48;
    return p;
}

// One-entry memo of the permutation table.
//
// A cache, not state: the table is a pure function of the seed, so a caller sees
// identical heights whether or not it hits. It exists because the height function
// is called around a million times during a build (every tile vertex, four more
// per normal, every collider vertex, every scatter attempt) and a Fisher-Yates
// shuffle per call would dominate load time entirely.
//
// Deliberately not thread-safe. Terrain is built once at load on the thread that
// owns the GL context; making this safe would mean handing every caller a table
// it has no other reason to know about.
static NoisePerm g_perm;
static unsigned g_perm_seed;
static bool g_perm_ready;

static const NoisePerm* perm_for(unsigned seed) {
    if (!g_perm_ready || g_perm_seed != seed) {
        noise_perm_init(&g_perm, seed);
        g_perm_seed = seed;
        g_perm_ready = true;
    }
    return &g_perm;
}

static float smoothstep01(float edge0, float edge1, float x) {
    if (edge1 <= edge0)
        return x < edge0 ? 0.0f : 1.0f;
    float t = (x - edge0) / (edge1 - edge0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

float terrain_height_at(const TerrainParams* p, float x, float z) {
    if (!p || p->octaves <= 0)
        return 0.0f;
    const NoisePerm* t = perm_for(p->seed);

    // Offset into positive coordinates before scaling: the lattice is indexed
    // from zero, and feeding it negatives would fold the terrain about its own
    // origin rather than continuing it.
    float ox = x + p->extent;
    float oz = z + p->extent;

    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = p->base_freq;
    for (int i = 0; i < p->octaves; ++i) {
        // A different constant Y plane per octave, the trick grass.c uses, so
        // octaves do not share ridge lines and stack into visible creases.
        float n = noise_perlin3_tiled(t, ox * freq, (float)i * 7.31f, oz * freq,
                                      TERRAIN_NOISE_PERIOD);
        sum += amp * n;
        norm += amp;
        amp *= p->gain;
        freq *= p->lacunarity;
    }
    return norm > 0.0f ? (sum / norm) * p->height : 0.0f;
}

// The step a normal is measured over: half a quad of the visual mesh, so the
// shading normal describes the surface the eye actually sees. A fixed epsilon
// would either sit under float precision on a large terrain or smooth away the
// detail the triangles carry.
static float normal_step(const TerrainParams* p) {
    int across = p->tiles * p->tile_segments;
    if (across <= 0)
        return 1.0f;
    return (2.0f * p->extent) / (float)across * 0.5f;
}

void terrain_normal_at(const TerrainParams* p, float x, float z, vec3 out) {
    if (!p) {
        glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, out);
        return;
    }
    float h = normal_step(p);
    float dx = terrain_height_at(p, x + h, z) - terrain_height_at(p, x - h, z);
    float dz = terrain_height_at(p, x, z + h) - terrain_height_at(p, x, z - h);
    vec3 n = {-dx, 2.0f * h, -dz};
    glm_vec3_normalize_to(n, out);
}

static void terrain_tint(const TerrainParams* p, float height, const vec3 normal, float* rgba) {
    float slope = normal[1]; // 1 flat, 0 vertical
    float alt = p->height > 0.0f ? height / p->height : 0.0f;

    float rockiness = 1.0f - smoothstep01(0.62f, 0.88f, slope);
    // Snow needs altitude AND a surface shallow enough to hold it, or peaks come
    // out frosted on their overhangs.
    float snowiness = smoothstep01(0.34f, 0.62f, alt) * smoothstep01(0.55f, 0.80f, slope);
    float dirtiness = 1.0f - smoothstep01(-0.35f, -0.05f, alt);

    vec3 c;
    glm_vec3_lerp((float*)TINT_GRASS, (float*)TINT_DIRT, dirtiness, c);
    glm_vec3_lerp(c, (float*)TINT_ROCK, rockiness, c);
    glm_vec3_lerp(c, (float*)TINT_SNOW, snowiness, c);

    rgba[0] = c[0];
    rgba[1] = c[1];
    rgba[2] = c[2];
    rgba[3] = 1.0f;
}

// The shared grid builder. Both the visual tiles and the collider are a regular
// XZ lattice sampled off the same height function; they differ only in extent,
// resolution, and whether anything will ever shade them.
static bool build_grid(const TerrainParams* p, float x0, float z0, float span, int segments,
                       bool shaded, Mesh* mesh) {
    if (segments <= 0)
        return false;
    int side = segments + 1;

    MeshBuilder mb;
    if (!mb_init(&mb, (size_t)side * (size_t)side, (size_t)segments * (size_t)segments * 6u,
                 shaded))
        return false;

    float step = span / (float)segments;
    // UV0 in world units, so detail tiles at a constant density whatever the tile
    // size is and runs continuously across a tile seam.
    const float uv_per_unit = 0.05f;

    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            float x = x0 + step * (float)i;
            float z = z0 + step * (float)j;
            float y = terrain_height_at(p, x, z);

            vec3 n = {0.0f, 1.0f, 0.0f};
            vec3 t = {1.0f, 0.0f, 0.0f};
            float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (shaded) {
                terrain_normal_at(p, x, z, n);
                // Gram-Schmidt +X against the normal. Terrain normals point
                // broadly up, so the two can never be parallel here.
                glm_vec3_muladds(n, -glm_vec3_dot(n, t), t);
                glm_vec3_normalize(t);
                terrain_tint(p, y, n, rgba);
            }

            vec3 pos = {x, y, z};
            mb_vertex(&mb, pos, n, t, x * uv_per_unit, z * uv_per_unit, 0.0f, 0.0f, rgba);
        }
    }

    for (int j = 0; j < segments; ++j) {
        for (int i = 0; i < segments; ++i) {
            unsigned int a = (unsigned int)(j * side + i);
            unsigned int b = a + 1u;
            unsigned int c = a + (unsigned int)side;
            unsigned int d = c + 1u;
            // (a, c, b) and (b, c, d) both wind to +Y.
            mb_tri(&mb, a, c, b);
            mb_tri(&mb, b, c, d);
        }
    }

    return mb_transfer(&mb, mesh);
}

bool terrain_build_tile(const TerrainParams* p, int tx, int tz, Mesh* mesh) {
    if (!p || !mesh || p->tiles <= 0 || p->tile_segments <= 0)
        return false;
    if (tx < 0 || tz < 0 || tx >= p->tiles || tz >= p->tiles)
        return false;

    float tile_span = (2.0f * p->extent) / (float)p->tiles;
    float x0 = -p->extent + tile_span * (float)tx;
    float z0 = -p->extent + tile_span * (float)tz;
    return build_grid(p, x0, z0, tile_span, p->tile_segments, true, mesh);
}

bool terrain_build_collider(const TerrainParams* p, int segments, Mesh* mesh) {
    if (!p || !mesh)
        return false;
    return build_grid(p, -p->extent, -p->extent, 2.0f * p->extent, segments, false, mesh);
}
