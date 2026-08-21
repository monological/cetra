#include <math.h>
#include <stdio.h>
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
//
// THESE ARE DISPLAY-SPACE NUMBERS, NOT LINEAR ALBEDOS, and the distinction is
// worth stating because this comment used to claim the opposite -- "measured-ish
// albedos, grass around 0.2". pbr_frag runs sRGBToLinear over VertexColor
// (pbr_frag.glsl:1036), so 0.15 arrives at the BRDF as 0.019. They were tuned by
// eye through that decode and are correct as they stand; what was wrong was the
// label. Re-tuning them to be literal albedos would relight the whole app for a
// documentation claim, so it is deliberately not done here.
static const vec3 TINT_GRASS = {0.15f, 0.21f, 0.10f};
static const vec3 TINT_MOSS = {0.11f, 0.17f, 0.08f};
static const vec3 TINT_DIRT = {0.25f, 0.20f, 0.14f};
static const vec3 TINT_ROCK = {0.30f, 0.29f, 0.27f};
static const vec3 TINT_SNOW = {0.78f, 0.81f, 0.86f};
// Only reachable through an erosion bake: pale outwash where the sim dropped its
// load, dark wet gravel where water collected and kept running.
static const vec3 TINT_SILT = {0.34f, 0.30f, 0.23f};
static const vec3 TINT_CHANNEL = {0.15f, 0.14f, 0.12f};

TerrainParams terrain_default_params(void) {
    TerrainParams p;
    p.extent = 500.0f;
    glm_vec2_zero(p.center);
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
    p.layered = false;
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

// A plane's node, with the border CLAMPED -- a terrain's edge is an edge, not a
// period. Shared by the sampler and the pyramid so the two cannot disagree
// about what lies outside.
static float plane_texel(const float* plane, int res, int i, int j) {
    i = i < 0 ? 0 : (i >= res ? res - 1 : i);
    j = j < 0 ? 0 : (j >= res ? res - 1 : j);
    return plane[(size_t)j * (size_t)res + (size_t)i];
}

// Level 0 borrows the field's own arrays; every level above it owns four.
static void field_free_levels(TerrainField* field) {
    for (int k = 1; k < field->level_count; ++k) {
        free(field->levels[k].height);
        free(field->levels[k].flow);
        free(field->levels[k].deposit);
        free(field->levels[k].wear);
    }
    free(field->levels);
    field->levels = NULL;
    field->level_count = 0;
}

/*
 * Half-resolution `src`, on the even nodes, under a separable [1 2 1] tent.
 *
 * The tent and not a plain subsample, and the plan said the opposite. Its
 * argument was that a coarse node must be BIT-EQUAL to the fine node under it,
 * because the CDLOD morph target was going to be built by averaging a patch's own
 * even vertices -- which is only the parent's surface if the two read the same
 * numbers. That constraint is gone: the target is sampled at the PARENT'S level
 * now, so it is the parent's surface by construction whatever the levels contain.
 *
 * And a subsample delivers nothing once the lattices align, which they do
 * whenever a patch cell is a whole multiple of a field cell: a coarse mesh
 * reading level 0 at every other node gets exactly the values level 1 stores, so
 * the pyramid is an exact no-op. What a coarse mesh needs is the detail REMOVED,
 * not addressed more cheaply -- a point sample every eight cells is not a
 * smoother surface, it is an arbitrary one, and it changes for no reason as the
 * camera moves.
 *
 * Node-centred, so the tent is clamped at the border rather than wrapped: a
 * terrain's edge is an edge, not a period.
 */
static bool filter_plane(const float* src, int src_res, float** out, int res) {
    float* dst = malloc((size_t)res * (size_t)res * sizeof(float));
    if (!dst)
        return false;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            float sum = 0.0f;
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    float w = (di == 0 ? 0.5f : 0.25f) * (dj == 0 ? 0.5f : 0.25f);
                    sum += w * plane_texel(src, src_res, i * 2 + di, j * 2 + dj);
                }
            }
            dst[(size_t)j * (size_t)res + (size_t)i] = sum;
        }
    }
    *out = dst;
    return true;
}

int terrain_field_build_pyramid(TerrainField* field) {
    if (!field || !field->height || field->res < 2)
        return 0;
    field_free_levels(field);

    // How many halvings the node count admits, stopping while a level still has
    // enough nodes for the 4-tap kernel to mean anything.
    int count = 1;
    for (int res = field->res; ((res - 1) & 1) == 0 && (res - 1) / 2 + 1 >= 5; ++count)
        res = (res - 1) / 2 + 1;

    field->levels = calloc((size_t)count, sizeof(TerrainFieldLevel));
    if (!field->levels)
        return 0;
    field->level_count = 1;
    field->levels[0].res = field->res;
    field->levels[0].height = field->height;
    field->levels[0].flow = field->flow;
    field->levels[0].deposit = field->deposit;
    field->levels[0].wear = field->wear;

    for (int k = 1; k < count; ++k) {
        const TerrainFieldLevel* prev = &field->levels[k - 1];
        TerrainFieldLevel* cur = &field->levels[k];
        cur->res = (prev->res - 1) / 2 + 1;
        if (!filter_plane(prev->height, prev->res, &cur->height, cur->res) ||
            !filter_plane(prev->flow, prev->res, &cur->flow, cur->res) ||
            !filter_plane(prev->deposit, prev->res, &cur->deposit, cur->res) ||
            !filter_plane(prev->wear, prev->res, &cur->wear, cur->res)) {
            // Keep what did build. A short pyramid samples coarser geometry at a
            // finer level than it wanted, which aliases; an absent one would make
            // every caller branch.
            free(cur->height);
            free(cur->flow);
            free(cur->deposit);
            free(cur->wear);
            memset(cur, 0, sizeof(*cur));
            break;
        }
        field->level_count = k + 1;
    }
    return field->level_count;
}

void terrain_field_measure(TerrainField* field) {
    if (!field || !field->height || field->res < 1) {
        if (field) {
            field->min_y = 0.0f;
            field->max_y = 0.0f;
        }
        return;
    }
    size_t n = (size_t)field->res * (size_t)field->res;
    float lo = field->height[0], hi = field->height[0];
    for (size_t k = 1; k < n; k++) {
        if (field->height[k] < lo)
            lo = field->height[k];
        if (field->height[k] > hi)
            hi = field->height[k];
    }
    field->min_y = lo;
    field->max_y = hi;
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

    for (int j = 0; j < field->res; ++j) {
        // terrain_field_node, not a hand-written `-extent + span*j/last`. Those
        // are the same value in exact arithmetic and NOT in floats -- the sampler
        // divides by `cell`, so the seed multiplies by the same `cell` and a node
        // pushed through the sampler lands on its own index. Re-associating it
        // moved the bake's digest and nothing else, which is how the two halves
        // of that claim were told apart: with the old seed restored, the thermal
        // rewrite below reads bit-identical to the version it replaced.
        float z = terrain_world_z(params, terrain_field_node(params->extent, field->res, j));
        for (int i = 0; i < field->res; ++i) {
            float x = terrain_world_x(params, terrain_field_node(params->extent, field->res, i));
            field->height[(size_t)j * (size_t)field->res + (size_t)i] =
                terrain_height_at(&analytic, x, z);
        }
    }
    terrain_field_measure(field);
    return true;
}

void terrain_field_free(TerrainField* field) {
    if (!field)
        return;
    field_free_levels(field);
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

// Shared by the height and all three masks so a mapping cannot drift between
// them: a mask read at a different place from the height it describes would put
// the silt somewhere other than where the sim deposited it.
static float sample_plane(const TerrainParams* p, const float* plane, int res, float x, float z) {
    // res >= 2 and a non-NULL height plane are terrain_field_alloc's invariants,
    // so they are not re-asked here. `plane` can still be NULL -- it is the
    // fall-through for a TerrainMask outside the enum -- and `extent` is
    // caller-owned and never validated on the way in.
    float cell = terrain_field_cell(p->extent, res);
    if (!plane || !(cell > 0.0f))
        return 0.0f;

    // Texels sit on NODES: 0 lands on -extent and res-1 on +extent. A coarse
    // level's node 0 is the fine level's node 0 and its last is the fine last,
    // so every level spans exactly the same square.
    float last = (float)(res - 1);
    float dx, dz;
    terrain_to_domain(p, x, z, &dx, &dz);
    float gx = dx / cell;
    float gz = dz / cell;
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
        rows[r] = catmull_rom(plane_texel(plane, res, i - 1, jr), plane_texel(plane, res, i, jr),
                              plane_texel(plane, res, i + 1, jr), plane_texel(plane, res, i + 2, jr),
                              tx);
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
    // Clamped because the sampler is a CUBIC: Catmull-Rom overshoots either side
    // of a sharp step, so a mask that is 0 across a whole neighbourhood still
    // reads slightly negative next to a peak. The header promises [0,1] and four
    // consumers would each have to re-establish it otherwise.
    float v = sample_plane(p, plane, p->field->res, x, z);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * What the ground is MADE OF here, in [0,1] per class: bare rock, dropped silt,
 * channel bed. Grass is whatever is left.
 *
 * One function because there are two consumers and they must not disagree: the
 * splat map decides what a layered terrain looks like, the vertex tint decides
 * what an un-layered one looks like, and the scatter decides what may stand on
 * either. All four thresholds lived in both consumers as literals, so a re-tune
 * of the tint would silently stop matching the layer under it -- and the two are
 * never live in the same frame, so there would be no in-frame contradiction to
 * notice.
 *
 * `extra_rock` is the tint's patch-field boost, folded in before the max with
 * wear so the combination stays exactly what it was. The splat passes 0.
 */
static void terrain_ground_classes(const TerrainParams* p, float x, float z, float slope,
                                   float extra_rock, float out[3]) {
    float wear = terrain_mask_at(p, TERRAIN_MASK_WEAR, x, z);
    float deposit = terrain_mask_at(p, TERRAIN_MASK_DEPOSIT, x, z);
    float flow = terrain_mask_at(p, TERRAIN_MASK_FLOW, x, z);

    float steep = 1.0f - smoothstep01(0.62f, 0.88f, slope);
    steep = steep + (1.0f - steep) * extra_rock;
    // Combined with max rather than added: a steep face that also eroded is not
    // twice as rocky.
    float scoured = smoothstep01(0.10f, 0.45f, wear);
    out[0] = steep > scoured ? steep : scoured;
    out[1] = smoothstep01(0.05f, 0.28f, deposit);
    out[2] = smoothstep01(TERRAIN_CHANNEL_FLOW_LO, TERRAIN_CHANNEL_FLOW_HI, flow);
}

bool terrain_bake_splat(const TerrainParams* p, int res, unsigned char* out_rgb) {
    if (!p || !out_rgb || res < 2)
        return false;

    // TEXEL CENTRES, not field nodes, and the difference is the whole reason
    // this loop does not call terrain_field_node. A texture read of texel j
    // happens at (j + 0.5) / res; a node sits at j / (res - 1). Sampling on
    // nodes and reading at centres is a half-texel shift AND a (res-1)/res
    // stretch between the splat and the terrain it describes.
    float texel = (2.0f * p->extent) / (float)res;
    for (int j = 0; j < res; j++) {
        float z = terrain_world_z(p, -p->extent + ((float)j + 0.5f) * texel);
        for (int i = 0; i < res; i++) {
            float x = terrain_world_x(p, -p->extent + ((float)i + 0.5f) * texel);

            vec3 n = {0.0f, 1.0f, 0.0f};
            terrain_normal_at(p, x, z, n);

            float cls[3];
            terrain_ground_classes(p, x, z, n[1], 0.0f, cls);
            float rock = cls[0], silt = cls[1], gravel = cls[2];

            // A channel bed is a channel bed even where it is also flat and
            // silty, so gravel wins its share outright and the other two divide
            // what is left. Without this a stream through a depositional fan
            // comes out as silt, which is the one place it certainly is not.
            float room = 1.0f - gravel;
            rock *= room;
            silt *= room;
            float sum = rock + silt + gravel;
            if (sum > 1.0f) {
                float inv = 1.0f / sum;
                rock *= inv;
                silt *= inv;
                gravel *= inv;
            }

            size_t o = ((size_t)j * (size_t)res + (size_t)i) * 3u;
            out_rgb[o + 0] = (unsigned char)(rock * 255.0f + 0.5f);
            out_rgb[o + 1] = (unsigned char)(silt * 255.0f + 0.5f);
            out_rgb[o + 2] = (unsigned char)(gravel * 255.0f + 0.5f);
        }
    }
    return true;
}

// The field level whose planes to read, and the resolution that goes with it.
// Level 0 with no pyramid built, which is every field from before 11.63.
static const TerrainFieldLevel* field_level(const TerrainField* f, int level) {
    static TerrainFieldLevel base;
    if (f->level_count > 0 && f->levels) {
        int k = level < 0 ? 0 : (level >= f->level_count ? f->level_count - 1 : level);
        return &f->levels[k];
    }
    base.res = f->res;
    base.height = f->height;
    base.flow = f->flow;
    base.deposit = f->deposit;
    base.wear = f->wear;
    return &base;
}

// The fbm's finest surviving octave at a level, as an octave COUNT.
//
// Nyquist on the octave's own wavelength: an octave of frequency f has features
// about 1/f across, and a lattice of `cell` cannot carry anything under 2*cell.
// Level 0 asks for cell 0 and keeps everything, which is what makes it identical
// to the function that has no level at all.
static int analytic_octaves(const TerrainParams* p, float cell) {
    if (cell <= 0.0f)
        return p->octaves;
    float freq = p->base_freq;
    int kept = 0;
    for (int i = 0; i < p->octaves; ++i) {
        if (freq > 0.0f && 1.0f / freq < 2.0f * cell)
            break;
        kept++;
        freq *= p->lacunarity;
    }
    // At least one, so a level coarser than the terrain's largest feature is
    // still a surface rather than a flat plane at zero.
    return kept > 0 ? kept : 1;
}

float terrain_level_cell(const TerrainParams* p, int level) {
    if (!p)
        return 0.0f;
    if (level < 0)
        level = 0;
    if (p->field && p->field->height) {
        const TerrainFieldLevel* L = field_level(p->field, level);
        return terrain_field_cell(p->extent, L->res);
    }
    if (p->octaves <= 0 || !(p->base_freq > 0.0f))
        return 0.0f;
    // Half the finest octave's wavelength: the smallest thing the fbm has to
    // resolve, which is what level 0 means when there is no grid to ask.
    float finest = p->base_freq;
    for (int i = 1; i < p->octaves; ++i)
        finest *= p->lacunarity;
    return (0.5f / finest) * (float)(1 << level);
}

int terrain_level_count(const TerrainParams* p) {
    if (!p)
        return 1;
    if (p->field && p->field->height)
        return p->field->level_count > 0 ? p->field->level_count : 1;
    return p->octaves > 0 ? p->octaves : 1;
}

int terrain_level_for_cell(const TerrainParams* p, float cell) {
    int count = terrain_level_count(p);
    int best = 0;
    for (int k = 1; k < count; ++k) {
        float lc = terrain_level_cell(p, k);
        if (!(lc > 0.0f) || lc > cell)
            break;
        best = k;
    }
    return best;
}

float terrain_height_at_level(const TerrainParams* p, float x, float z, int level) {
    if (!p)
        return 0.0f;
    // Tested before the octave count, deliberately: a field with octaves 0 is a
    // legitimate configuration -- the fbm is simply inert -- and folding the two
    // guards together would return a flat 0 for it.
    if (p->field && p->field->height) {
        const TerrainFieldLevel* L = field_level(p->field, level);
        return sample_plane(p, L->height, L->res, x, z);
    }
    if (p->octaves <= 0)
        return 0.0f;
    const NoisePerm* t = perm_for(p->seed);

    // Offset into positive coordinates before scaling: the lattice is indexed
    // from zero, and feeding it negatives would fold the terrain about its own
    // origin rather than continuing it.
    float ox, oz;
    terrain_to_domain(p, x, z, &ox, &oz);

    int kept = level > 0 ? analytic_octaves(p, terrain_level_cell(p, level)) : p->octaves;
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = p->base_freq;
    for (int i = 0; i < p->octaves; ++i) {
        // A different constant Y plane per octave, the trick grass.c uses, so
        // octaves do not share ridge lines and stack into visible creases.
        //
        // The dropped octaves still count toward `norm`. A level is the surface
        // MINUS what it cannot resolve; renormalising to the kept octaves would
        // scale the survivors back up to full amplitude and make each level a
        // different terrain rather than a smoother view of one.
        if (i < kept) {
            float n = noise_perlin3_tiled(t, ox * freq, (float)i * 7.31f, oz * freq,
                                          TERRAIN_NOISE_PERIOD);
            sum += amp * n;
        }
        norm += amp;
        amp *= p->gain;
        freq *= p->lacunarity;
    }
    return norm > 0.0f ? (sum / norm) * p->height : 0.0f;
}

float terrain_height_at(const TerrainParams* p, float x, float z) {
    return terrain_height_at_level(p, x, z, 0);
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

void terrain_normal_at_level(const TerrainParams* p, float x, float z, int level, vec3 out) {
    if (!p) {
        glm_vec3_copy((vec3){0.0f, 1.0f, 0.0f}, out);
        return;
    }
    // Level 0 keeps normal_step exactly, so nothing built before levels existed
    // moves. Above it the step widens to the level's own half-cell where that is
    // larger: differencing a coarse surface over a fine step measures the
    // interpolant's slope rather than the surface's.
    float h = normal_step(p);
    if (level > 0) {
        float half = terrain_level_cell(p, level) * 0.5f;
        if (half > h)
            h = half;
    }
    float dx = terrain_height_at_level(p, x + h, z, level) -
               terrain_height_at_level(p, x - h, z, level);
    float dz = terrain_height_at_level(p, x, z + h, level) -
               terrain_height_at_level(p, x, z - h, level);
    vec3 n = {-dx, 2.0f * h, -dz};
    glm_vec3_normalize_to(n, out);
}

void terrain_normal_at(const TerrainParams* p, float x, float z, vec3 out) {
    terrain_normal_at_level(p, x, z, 0, out);
}

// The macro-variation half of the tint, for a layered material (spec 11.60).
//
// Grey, not coloured, and centred so the BRIGHT end is exactly 1.0 -- pbr_frag
// multiplies albedo by this, so anything above white would brighten a layer past
// what it authored and the layer set would stop meaning what it says. The band
// is stated in DISPLAY space because pbr_frag runs sRGBToLinear over vertex
// colour: 0.94 arrives at the BRDF as 0.87, so the swing is about 13% and not
// the 6% the numbers look like.
#define TERRAIN_MACRO_LO 0.94f
#define TERRAIN_MACRO_HI 1.0f

static void terrain_macro(const TerrainParams* p, float x, float z, float* rgba) {
    const NoisePerm* t = perm_for(p->seed);
    float ox, oz;
    terrain_to_domain(p, x, z, &ox, &oz);
    // The LOW-frequency field alone. The fine one exists to break up a flat hue,
    // which is a job the layer maps now do at their own resolution and do better;
    // what is left for 2.6-unit vertices is the drift a tiling texture cannot
    // have at all.
    float patch = noise_perlin3_tiled(t, ox * 0.011f, 11.3f, oz * 0.011f, TERRAIN_NOISE_PERIOD);
    float v = 0.5f + 0.5f * patch;
    float lift = TERRAIN_MACRO_LO + (TERRAIN_MACRO_HI - TERRAIN_MACRO_LO) * v;
    rgba[0] = rgba[1] = rgba[2] = lift;
    rgba[3] = 1.0f;
}

static void terrain_tint(const TerrainParams* p, float x, float z, float height, const vec3 normal,
                         float* rgba) {
    if (p->layered) {
        terrain_macro(p, x, z, rgba);
        return;
    }

    float slope = normal[1]; // 1 flat, 0 vertical

    // Altitude in [-1, 1], against whatever is actually setting the relief.
    //
    // params->height is the fbm's AMPLITUDE, and it goes inert the moment a field
    // is installed -- so using it for a loaded field measured altitude against a
    // number with no relationship to the terrain. A Gaea export loaded over
    // 0..1200 into an app whose params say 95 read alt up to 12.6, which put
    // every surface above y = 32 fully under snow and left the dirt band
    // unreachable. A field states its own range, and that is what normalises here.
    float relief = p->height;
    float centre = 0.0f;
    if (p->field && p->field->max_y > p->field->min_y) {
        relief = 0.5f * (p->field->max_y - p->field->min_y);
        centre = 0.5f * (p->field->max_y + p->field->min_y);
    }
    float alt = relief > 0.0f ? (height - centre) / relief : 0.0f;

    // Two extra noise fields, at frequencies unrelated to the height's, so the
    // ground does not simply restate its own shape in colour. Without them every
    // vertex at one slope and altitude is the same hue, and a kilometre of it
    // reads as a single flat green whatever the palette.
    const NoisePerm* t = perm_for(p->seed);
    float ox, oz;
    terrain_to_domain(p, x, z, &ox, &oz);
    float patch = noise_perlin3_tiled(t, ox * 0.011f, 11.3f, oz * 0.011f, TERRAIN_NOISE_PERIOD);
    float grain = noise_perlin3_tiled(t, ox * 0.09f, 23.9f, oz * 0.09f, TERRAIN_NOISE_PERIOD);

    // Snow needs altitude AND a surface shallow enough to hold it, or peaks come
    // out frosted on their overhangs.
    float snowiness = smoothstep01(0.34f, 0.62f, alt) * smoothstep01(0.55f, 0.80f, slope);
    float dirtiness = 1.0f - smoothstep01(-0.35f, -0.05f, alt);

    // WHERE WATER PUT THINGS, when a sim has run and said so, through the same
    // classification the splat map is baked from -- so the tinted look and the
    // layered look cannot drift apart.
    //
    // Everything else here guesses at the answer from shape alone, which is the
    // best a closed form can do and is why terrain shaded that way reads as
    // procedural: the dirt is not where water would put dirt.
    //
    // Every class reads 0 with no field installed except the slope term, and every
    // term below collapses to an exact identity at 0 -- smoothstep01 returns 0
    // below its low edge, and lerping by 0 returns the first operand unchanged in
    // IEEE. So the analytic path is byte-for-byte what it was.
    //
    // The patch boost rides in as `extra_rock`: bare ground creeps in where that
    // field is high, independent of slope.
    float cls[3];
    terrain_ground_classes(p, x, z, slope, smoothstep01(0.30f, 0.62f, patch) * 0.45f, cls);
    float rockiness = cls[0], siltiness = cls[1], wetness = cls[2];

    vec3 c;
    // Grass to moss first, at the fine frequency: it is what stops a hillside
    // of one hue from looking painted.
    glm_vec3_lerp((float*)TINT_GRASS, (float*)TINT_MOSS, grain * 0.5f + 0.5f, c);
    glm_vec3_lerp(c, (float*)TINT_DIRT, dirtiness, c);
    glm_vec3_lerp(c, (float*)TINT_ROCK, rockiness, c);
    // Silt after rock, because deposition BURIES bedrock -- an outwash fan over a
    // scoured shelf is sand, not stone with sand mixed in.
    glm_vec3_lerp(c, (float*)TINT_SILT, siltiness, c);
    // The channel last but for snow: a stream cutting through rock is still a
    // stream, so this wins over everything it runs across.
    glm_vec3_lerp(c, (float*)TINT_CHANNEL, wetness, c);
    glm_vec3_lerp(c, (float*)TINT_SNOW, snowiness, c);

    rgba[0] = c[0];
    rgba[1] = c[1];
    rgba[2] = c[2];
    rgba[3] = 1.0f;
}

// Sample points the height probe reports. A fixed set, because an arm asserting
// against ground truth needs to know where the ground truth was taken -- and a
// grid derived from the extent moves the moment a caller changes it.
#define PROBE_GRID 5

// Print what the height function actually returns, which nothing else can show.
// A field wired to the wrong world scale, read with the axes swapped, or clamped
// to zero outside its domain still renders as terrain.
//
// In the library beside the sampler it tests, not in an app: this is the falsifier
// for everything above, and every other probe in this tree -- water_fft_probe,
// wind_bound_probe, emissive_lights_probe -- sits beside its own data for the same
// reason. apps/forest is only the first terrain consumer, not the last.
void terrain_height_probe(const TerrainParams* p) {
    if (!p)
        return;
    const TerrainField* f = p->field;
    float extent = p->extent;
    printf("terrain-height-probe header source=%s res=%d extent=%.4f cell=%.6f\n",
           f ? "field" : "analytic", f ? f->res : 0, (double)extent,
           f ? (double)terrain_field_cell(extent, f->res) : 0.0);

    for (int j = 0; j < PROBE_GRID; ++j) {
        for (int i = 0; i < PROBE_GRID; ++i) {
            // Interior fractions, not the corners: a corner is where clamping and
            // sampling give the same answer, so it cannot tell them apart.
            float u = (float)(i + 1) / (float)(PROBE_GRID + 1);
            float v = (float)(j + 1) / (float)(PROBE_GRID + 1);
            float x = terrain_world_x(p, -extent + 2.0f * extent * u);
            float z = terrain_world_z(p, -extent + 2.0f * extent * v);
            // SNAPPED ONTO A NODE when there is a field, so these rows read the
            // stored value and not the interpolant. That splits two questions that
            // otherwise contaminate each other: whether the field is MAPPED right
            // -- axis order, node convention, world range -- and how well it is
            // FILTERED between nodes. On the lattice every correct sampler agrees
            // exactly, so a mapping error has nowhere to hide behind interpolation
            // error, and the bar can stay at a couple of quantisation codes.
            if (f) {
                float snap = terrain_field_cell(extent, f->res);
                float dx, dz;
                terrain_to_domain(p, x, z, &dx, &dz);
                x = terrain_world_x(p, terrain_field_node(extent, f->res,
                                                          (int)floorf(dx / snap + 0.5f)));
                z = terrain_world_z(p, terrain_field_node(extent, f->res,
                                                          (int)floorf(dz / snap + 0.5f)));
            }
            vec3 n = {0.0f, 1.0f, 0.0f};
            terrain_normal_at(p, x, z, n);
            // The fbm at the SAME point, alongside whatever the active source
            // says. With a field seeded from the fbm and no sim run over it the
            // two must agree, which is the node convention asserted end to end --
            // and comparing them in one process is what avoids matching
            // coordinates across two runs whose sample points do not coincide.
            TerrainParams analytic = *p;
            analytic.field = NULL;
            printf("terrain-height-probe sample x=%.4f z=%.4f h=%.6f fbm=%.6f ny=%.6f "
                   "flow=%.6f deposit=%.6f wear=%.6f\n",
                   (double)x, (double)z, (double)terrain_height_at(p, x, z),
                   (double)terrain_height_at(&analytic, x, z), (double)n[1],
                   (double)terrain_mask_at(p, TERRAIN_MASK_FLOW, x, z),
                   (double)terrain_mask_at(p, TERRAIN_MASK_DEPOSIT, x, z),
                   (double)terrain_mask_at(p, TERRAIN_MASK_WEAR, x, z));
        }
    }

    // Outside the domain, paired with the edge point each one should clamp ONTO.
    // The camera really does query out here, so "returns something finite" is not
    // the assertion -- "returns the edge" is.
    //
    // HALF OF THESE SIT LESS THAN A CELL OUTSIDE, and that is the whole reason the
    // set is not four far-away points. Catmull-Rom over four EQUAL taps returns
    // that value exactly, so once a query is far enough out that all four taps
    // clamp to the same edge column, an unclamped sampler returns the edge anyway
    // and cannot be told from a clamped one. The difference lives in the first
    // cell beyond the boundary, where the taps still differ.
    float step = f ? terrain_field_cell(extent, f->res) : 1.0f;
    float near_u = (extent + 0.4f * step) / extent;
    const float out[8][2] = {{-1.35f, 0.20f},   {1.35f, -0.40f}, {0.10f, 1.60f},
                             {-0.70f, -1.90f}, {-near_u, 0.15f}, {near_u, -0.55f},
                             {0.35f, near_u},  {-0.25f, -near_u}};
    for (int k = 0; k < 8; ++k) {
        float lx = out[k][0] * extent, lz = out[k][1] * extent;
        float x = terrain_world_x(p, lx), z = terrain_world_z(p, lz);
        float cx = terrain_world_x(p, lx < -extent ? -extent : (lx > extent ? extent : lx));
        float cz = terrain_world_z(p, lz < -extent ? -extent : (lz > extent ? extent : lz));
        printf("terrain-height-probe clamp x=%.4f z=%.4f h=%.6f edge_x=%.4f edge_z=%.4f "
               "edge_h=%.6f\n",
               (double)x, (double)z, (double)terrain_height_at(p, x, z), (double)cx,
               (double)cz, (double)terrain_height_at(p, cx, cz));
    }

    // CELL MIDPOINTS, which is where the filter is decided and the only place it
    // can be seen. Every interpolant agrees exactly at a node, so a probe that
    // samples on the lattice tests the storage and not the filter -- and a probe
    // that samples the NORMAL cannot see it either, because terrain_normal_at
    // central-differences over less than a cell and a bilinear surface is locally
    // the linearisation the difference is estimating. Halfway between two nodes is
    // where a chord is furthest from the curve it cuts.
    float cell = f ? terrain_field_cell(extent, f->res) : 1.0f;
    for (int k = 0; k < PROBE_GRID * PROBE_GRID; ++k) {
        int gi = k % PROBE_GRID, gj = k / PROBE_GRID;
        // Land on a node first, then step half a cell in x, so the offset is
        // exactly half however the extent and resolution are chosen.
        float u = (float)(gi + 1) / (float)(PROBE_GRID + 1);
        float v = (float)(gj + 1) / (float)(PROBE_GRID + 1);
        float nx = floorf((u * 2.0f * extent) / cell);
        float nz = floorf((v * 2.0f * extent) / cell);
        float x = terrain_world_x(p, -extent + (nx + 0.5f) * cell);
        float z = terrain_world_z(p, -extent + nz * cell);
        printf("terrain-height-probe mid x=%.6f z=%.6f h=%.6f\n", (double)x, (double)z,
               (double)terrain_height_at(p, x, z));
    }

}

// The shared grid builder. Both the visual tiles and the collider are a regular
// XZ lattice sampled off the same height function; they differ only in extent,
// resolution, and whether anything will ever shade them.
static bool build_grid(const TerrainParams* p, float x0, float z0, float span, int segments,
                       bool shaded, int level, Mesh* mesh) {
    if (segments <= 0)
        return false;
    int side = segments + 1;

    MeshBuilder mb;
    if (!mb_init(&mb, (size_t)side * (size_t)side, (size_t)segments * (size_t)segments * 6u,
                 shaded))
        return false;

    float step = span / (float)segments;
    // UV0 at a constant density in world units, so detail tiles the same whatever
    // the patch size is and runs continuously across a seam.
    //
    // Off the DOMAIN coordinate rather than the world one, which is spec 11.62's
    // rule 2: a lookup keyed to position is an IDENTITY, so it has to be the
    // authored position or the pattern slides when the world moves under it.
    // Nothing could notice while a tile was built once and then carried by its
    // node's transform; a quadtree patch is rebuilt at the new origin after a
    // shift, and a world-derived UV re-derives somewhere else. Latent here --
    // forest's terrain is layered and reads its detail through the splat, which
    // was already authored-space -- so the surface this protects is a terrain
    // that is NOT layered.
    const float uv_per_unit = 0.05f;

    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            float x = x0 + step * (float)i;
            float z = z0 + step * (float)j;
            float y = terrain_height_at_level(p, x, z, level);

            vec3 n = {0.0f, 1.0f, 0.0f};
            vec3 t = {1.0f, 0.0f, 0.0f};
            float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (shaded) {
                terrain_normal_at_level(p, x, z, level, n);
                // Gram-Schmidt +X against the normal. Terrain normals point
                // broadly up, so the two can never be parallel here.
                glm_vec3_muladds(n, -glm_vec3_dot(n, t), t);
                glm_vec3_normalize(t);
                terrain_tint(p, x, z, y, n, rgba);
            }

            vec3 pos = {x, y, z};
            float ux, uz;
            terrain_to_domain(p, x, z, &ux, &uz);
            mb_vertex(&mb, pos, n, t, ux * uv_per_unit, uz * uv_per_unit, 0.0f, 0.0f, rgba);
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
    float x0 = terrain_world_x(p, -p->extent + tile_span * (float)tx);
    float z0 = terrain_world_z(p, -p->extent + tile_span * (float)tz);
    return build_grid(p, x0, z0, tile_span, p->tile_segments, true, 0, mesh);
}

/*
 * The parent surface at every vertex, as the two morph attribute streams.
 *
 * The parent lattice restricted to this patch is its EVEN-indexed subset (see
 * terrain.h), so an even vertex morphs to itself and an odd one to the point of
 * the parent's own triangulation directly above or below it. Which parent point
 * that is depends on the winding build_grid uses: its quads split (a, c, b) and
 * (b, c, d), so the shared edge runs from (i+1, j) to (i, j+1) and a doubly-odd
 * vertex sits on the coarse quad's anti-diagonal rather than on its other one.
 *
 * Every case is a midpoint of two parent NODES, and every one has the same X and
 * Z as the vertex itself -- so the whole displacement is in Y, which is why the
 * attribute is one float and not three.
 *
 * The nodes are SAMPLED AT THE PARENT'S LEVEL rather than read off this patch's
 * own even vertices. The two are the same number only while both patches read
 * the same level, which they do not once a pyramid exists: the parent drops what
 * its coarser cell cannot carry, and a target averaged from this patch's
 * vertices would be a surface the parent never draws. The linear interpolation
 * between them is not an approximation of the parent -- it IS what the parent
 * rasterizes between two of its vertices.
 */
static void fill_morph_targets(const TerrainParams* p, Mesh* mesh, float x0, float z0, float span,
                               int segments, int parent_level, float morph_start,
                               float morph_end) {
    int side = segments + 1;
    int cside = segments / 2 + 1;
    size_t n = mesh->vertex_count;
    float* morph = malloc(n * 3 * sizeof(float));
    float* normals = malloc(n * 3 * sizeof(float));
    float* ph = malloc((size_t)cside * (size_t)cside * sizeof(float));
    float* pn = malloc((size_t)cside * (size_t)cside * 3 * sizeof(float));
    if (!morph || !normals || !ph || !pn) {
        free(morph);
        free(normals);
        free(ph);
        free(pn);
        return;
    }

    // The parent's own nodes over this square, which are this patch's even ones.
    float step = span / (float)segments;
    for (int cj = 0; cj < cside; ++cj) {
        for (int ci = 0; ci < cside; ++ci) {
            float x = x0 + step * (float)(ci * 2);
            float z = z0 + step * (float)(cj * 2);
            size_t c = (size_t)cj * (size_t)cside + (size_t)ci;
            ph[c] = terrain_height_at_level(p, x, z, parent_level);
            vec3 nrm = {0.0f, 1.0f, 0.0f}; // out-param; seeded for static analysis
            terrain_normal_at_level(p, x, z, parent_level, nrm);
            glm_vec3_copy(nrm, &pn[c * 3]);
        }
    }

    float inv_span = morph_end > morph_start ? 1.0f / (morph_end - morph_start) : 0.0f;
    float worst = 0.0f;

    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            int v = j * side + i;
            // The two parent nodes this vertex lies between, as an offset from
            // it. Zero on the even/even case, which makes that vertex its own
            // target by the same arithmetic rather than by a special case.
            int di = 0, dj = 0;
            if (i & 1) {
                di = 1;
                dj = (j & 1) ? -1 : 0;
            } else if (j & 1) {
                dj = 1;
            }
            size_t a = (size_t)((j + dj) / 2) * (size_t)cside + (size_t)((i + di) / 2);
            size_t b = (size_t)((j - dj) / 2) * (size_t)cside + (size_t)((i - di) / 2);

            float y = 0.5f * (ph[a] + ph[b]);
            morph[v * 3 + 0] = y;
            morph[v * 3 + 1] = morph_start;
            morph[v * 3 + 2] = inv_span;

            vec3 nrm;
            glm_vec3_add(&pn[a * 3], &pn[b * 3], nrm);
            glm_vec3_normalize(nrm);
            glm_vec3_copy(nrm, &normals[v * 3]);

            float d = fabsf(y - mesh->vertices[v * 3 + 1]);
            if (d > worst)
                worst = d;
        }
    }

    free(ph);
    free(pn);
    mesh->morph = morph;
    mesh->morph_normals = normals;
    // Exactly reached at factor 1, so the culler's box is tight rather than
    // merely safe -- the morph is a lerp between two stored values.
    mesh->morph_max_offset = worst;
}

bool terrain_build_patch(const TerrainParams* p, float x0, float z0, float span, int segments,
                         float morph_start, float morph_end, Mesh* mesh) {
    if (!p || !mesh || segments < 2 || (segments & 1))
        return false;
    // Both levels come from the CELL rather than from a depth index, which is
    // what keeps a patch and its parent one level apart without either knowing
    // the other exists: the parent patch is built at twice this span with the
    // same segment count, so it asks for exactly the cell asked for here.
    float cell = span / (float)segments;
    int level = terrain_level_for_cell(p, cell);
    int parent_level = terrain_level_for_cell(p, cell * 2.0f);
    if (!build_grid(p, x0, z0, span, segments, true, level, mesh))
        return false;
    fill_morph_targets(p, mesh, x0, z0, span, segments, parent_level, morph_start, morph_end);
    return true;
}

bool terrain_build_collider(const TerrainParams* p, int segments, Mesh* mesh) {
    if (!p || !mesh)
        return false;
    return build_grid(p, terrain_world_x(p, -p->extent), terrain_world_z(p, -p->extent),
                      2.0f * p->extent, segments, false, 0, mesh);
}
