#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "terrain.h"

#include "../mesh_builder.h"
#include "../noise.h"

// The tiling lattice period noise_perlin3_tiled works over. Coordinates are
// offset to start at zero and the default frequencies keep the finest octave
// well inside one period across the whole terrain, so the tiling never shows.
#define TERRAIN_NOISE_PERIOD 256

// Surface tints, blended per vertex. There is no splat-map system, so this is
// what stops a kilometre of ground reading as one flat hue.
// Measured-ish albedos rather than picked-by-eye ones. Exposure is pinned, so
// these land where a real surface would: grass around 0.2, dry earth 0.25,
// weathered granite 0.3.
static const vec3 TINT_GRASS = {0.15f, 0.21f, 0.10f};
static const vec3 TINT_MOSS = {0.11f, 0.17f, 0.08f};
static const vec3 TINT_DIRT = {0.25f, 0.20f, 0.14f};
static const vec3 TINT_ROCK = {0.30f, 0.29f, 0.27f};
static const vec3 TINT_SNOW = {0.78f, 0.81f, 0.86f};

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
    p.field = NULL;
    return p;
}

bool terrain_field_alloc(TerrainField* field, int res) {
    if (!field)
        return false;
    memset(field, 0, sizeof(*field));
    if (res < 2)
        return false;

    size_t n = (size_t)res * (size_t)res;
    field->height = calloc(n, sizeof(float));
    field->flow = calloc(n, sizeof(float));
    field->deposit = calloc(n, sizeof(float));
    field->wear = calloc(n, sizeof(float));
    if (!field->height || !field->flow || !field->deposit || !field->wear) {
        terrain_field_free(field);
        return false;
    }
    field->res = res;
    return true;
}

bool terrain_field_seed(TerrainField* field, const TerrainParams* params) {
    if (!field || !field->height || field->res < 2 || !params)
        return false;
    if (!(params->extent > 0.0f))
        return false;

    // Evaluate through a copy with no field installed, so seeding a field that is
    // ALREADY installed on the caller's params reads the fbm rather than reading
    // the plane it is about to overwrite.
    TerrainParams analytic = *params;
    analytic.field = NULL;

    float span = 2.0f * params->extent;
    float last = (float)(field->res - 1);
    for (int j = 0; j < field->res; ++j) {
        float z = -params->extent + span * (float)j / last;
        for (int i = 0; i < field->res; ++i) {
            float x = -params->extent + span * (float)i / last;
            field->height[(size_t)j * (size_t)field->res + (size_t)i] =
                terrain_height_at(&analytic, x, z);
        }
    }
    return true;
}

void terrain_field_free(TerrainField* field) {
    if (!field)
        return;
    free(field->height);
    free(field->flow);
    free(field->deposit);
    free(field->wear);
    memset(field, 0, sizeof(*field));
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

// Catmull-Rom over four samples. Bicubic rather than bilinear, and the reason is
// terrain_normal_at rather than the look of the surface: that function central-
// differences this one over half a visual quad -- 1.30 units at the default
// sizing, the same order as a field cell -- and bilinear's derivative is piecewise
// constant, so the difference would step at every cell boundary. Those facets land
// in the shading AND in the scatter's slope gate, where a rejected band reads as
// rows of missing trees rather than as a filtering choice.
static float catmull_rom(float a, float b, float c, float d, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * (2.0f * b + (c - a) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
                   (3.0f * b - 3.0f * c + d - a) * t3);
}

static float plane_texel(const TerrainField* f, const float* plane, int i, int j) {
    i = i < 0 ? 0 : (i >= f->res ? f->res - 1 : i);
    j = j < 0 ? 0 : (j >= f->res ? f->res - 1 : j);
    return plane[(size_t)j * (size_t)f->res + (size_t)i];
}

// Shared by the height and all three masks so a mapping cannot drift between
// them: a mask read at a different place from the height it describes would put
// the silt somewhere other than where the sim deposited it.
static float sample_plane(const TerrainParams* p, const float* plane, float x, float z) {
    const TerrainField* f = p->field;
    float span = 2.0f * p->extent;
    if (!f || !plane || f->res < 2 || !(span > 0.0f))
        return 0.0f;

    // Texels sit on NODES: 0 lands on -extent and res-1 on +extent.
    float last = (float)(f->res - 1);
    float gx = (x + p->extent) / span * last;
    float gz = (z + p->extent) / span * last;
    // Clamp the COORDINATE rather than only the taps. Clamping taps alone still
    // interpolates between them with an out-of-range t, which extrapolates the
    // cubic past the edge -- the failure this policy exists to avoid.
    gx = gx < 0.0f ? 0.0f : (gx > last ? last : gx);
    gz = gz < 0.0f ? 0.0f : (gz > last ? last : gz);

    int i = (int)floorf(gx);
    int j = (int)floorf(gz);
    float tx = gx - (float)i;
    float tz = gz - (float)j;

    float rows[4];
    for (int r = 0; r < 4; ++r) {
        int jr = j - 1 + r;
        rows[r] = catmull_rom(plane_texel(f, plane, i - 1, jr), plane_texel(f, plane, i, jr),
                              plane_texel(f, plane, i + 1, jr), plane_texel(f, plane, i + 2, jr), tx);
    }
    return catmull_rom(rows[0], rows[1], rows[2], rows[3], tz);
}

float terrain_mask_at(const TerrainParams* p, TerrainMask mask, float x, float z) {
    if (!p || !p->field)
        return 0.0f;
    const float* plane = NULL;
    switch (mask) {
    case TERRAIN_MASK_FLOW:
        plane = p->field->flow;
        break;
    case TERRAIN_MASK_DEPOSIT:
        plane = p->field->deposit;
        break;
    case TERRAIN_MASK_WEAR:
        plane = p->field->wear;
        break;
    }
    return sample_plane(p, plane, x, z);
}

float terrain_height_at(const TerrainParams* p, float x, float z) {
    if (!p)
        return 0.0f;
    // Tested before the octave count, deliberately: a field with octaves 0 is a
    // legitimate configuration -- the fbm is simply inert -- and folding the two
    // guards together would return a flat 0 for it.
    if (p->field && p->field->height)
        return sample_plane(p, p->field->height, x, z);
    if (p->octaves <= 0)
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

static void terrain_tint(const TerrainParams* p, float x, float z, float height, const vec3 normal,
                         float* rgba) {
    float slope = normal[1]; // 1 flat, 0 vertical
    float alt = p->height > 0.0f ? height / p->height : 0.0f;

    // Two extra noise fields, at frequencies unrelated to the height's, so the
    // ground does not simply restate its own shape in colour. Without them every
    // vertex at one slope and altitude is the same hue, and a kilometre of it
    // reads as a single flat green whatever the palette.
    const NoisePerm* t = perm_for(p->seed);
    float ox = x + p->extent, oz = z + p->extent;
    float patch = noise_perlin3_tiled(t, ox * 0.011f, 11.3f, oz * 0.011f, TERRAIN_NOISE_PERIOD);
    float grain = noise_perlin3_tiled(t, ox * 0.09f, 23.9f, oz * 0.09f, TERRAIN_NOISE_PERIOD);

    float rockiness = 1.0f - smoothstep01(0.62f, 0.88f, slope);
    // Bare ground creeps in where the patch field is high, independent of slope.
    rockiness = rockiness + (1.0f - rockiness) * smoothstep01(0.30f, 0.62f, patch) * 0.45f;
    // Snow needs altitude AND a surface shallow enough to hold it, or peaks come
    // out frosted on their overhangs.
    float snowiness = smoothstep01(0.34f, 0.62f, alt) * smoothstep01(0.55f, 0.80f, slope);
    float dirtiness = 1.0f - smoothstep01(-0.35f, -0.05f, alt);

    vec3 c;
    // Grass to moss first, at the fine frequency: it is what stops a hillside
    // of one hue from looking painted.
    glm_vec3_lerp((float*)TINT_GRASS, (float*)TINT_MOSS, grain * 0.5f + 0.5f, c);
    glm_vec3_lerp(c, (float*)TINT_DIRT, dirtiness, c);
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
                terrain_tint(p, x, z, y, n, rgba);
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
