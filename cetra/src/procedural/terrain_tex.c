#include <stdlib.h>

#include "terrain_tex.h"
#include "vegetation_tex.h"

// Noise frequencies are in CELLS, whole numbers, because the periodic variants
// wrap on the integer lattice and a fractional frequency has no period to wrap
// on. Every one of these is therefore an integer and not a tuning float.
typedef struct LayerRecipe {
    int relief_octaves;
    float relief_persistence;
    int relief_freq;   // fbm cells across the tile
    int cell_freq;     // worley cells across the tile; 0 = no cellular term
    float cell_weight; // how much of the relief the cellular term owns
    float base[3];     // albedo at mid relief, in stored codes / 255
    float shade;       // how far relief darkens and lightens the base
    float rough_lo;    // roughness in a trough
    float rough_hi;    // roughness on a crest
    float relief_scale; // normal strength, in code units per texel of slope
} LayerRecipe;

// The four grounds. These are the numbers that decide what the terrain looks
// like, which is why they sit together in one table rather than being spread
// through four near-identical functions.
static const LayerRecipe RECIPES[] = {
    // grass: fine and busy, matte everywhere, barely any relief contrast
    [TERRAIN_LAYER_GRASS] = {5, 0.55f, 16, 24, 0.35f, {0.17f, 0.21f, 0.11f}, 0.45f, 0.86f,
                             0.95f, 2.2f},
    // rock: fractured, so the cellular term dominates and the fbm only roughens it
    [TERRAIN_LAYER_ROCK] = {4, 0.50f, 8, 6, 0.75f, {0.31f, 0.30f, 0.28f}, 0.55f, 0.62f, 0.80f,
                            4.5f},
    // silt: smooth and pale, faint bedding from a low-octave fbm alone
    [TERRAIN_LAYER_SILT] = {3, 0.45f, 6, 0, 0.0f, {0.36f, 0.32f, 0.25f}, 0.30f, 0.70f, 0.82f,
                            1.4f},
    // gravel: coarse cells with real depth between them
    [TERRAIN_LAYER_GRAVEL] = {3, 0.60f, 12, 10, 0.85f, {0.26f, 0.25f, 0.23f}, 0.60f, 0.72f,
                              0.88f, 5.0f},
};

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static unsigned char to_code(float v) { return (unsigned char)(clamp01(v) * 255.0f + 0.5f); }

// The layer's relief in [0,1], one field behind every channel below.
static void relief_field(const LayerRecipe* r, float* out, int size, unsigned int seed) {
    float inv = 1.0f / (float)size;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float u = (float)x * inv;
            float v = (float)y * inv;
            float f = veg_fbm2_tiled(u * (float)r->relief_freq, v * (float)r->relief_freq,
                                     r->relief_octaves, r->relief_persistence, r->relief_freq,
                                     r->relief_freq);
            f = f * 0.5f + 0.5f;
            if (r->cell_freq > 0) {
                // Worley returns distance to the nearest feature point, so it is
                // LOW inside a cell and high at the boundary -- inverted here so
                // a gravel stone reads as raised rather than as a pit.
                float c = veg_worley2_tiled(u * (float)r->cell_freq, v * (float)r->cell_freq, seed,
                                            r->cell_freq, r->cell_freq);
                f = f * (1.0f - r->cell_weight) + (1.0f - clamp01(c)) * r->cell_weight;
            }
            out[y * size + x] = clamp01(f);
        }
    }
}

void terrain_layer_maps(TerrainLayerKind kind, int size, unsigned int seed,
                        unsigned char** out_albedo, unsigned char** out_surface) {
    if (out_albedo)
        *out_albedo = NULL;
    if (out_surface)
        *out_surface = NULL;
    if (size < 2 || kind < TERRAIN_LAYER_GRASS || kind > TERRAIN_LAYER_GRAVEL)
        return;

    const LayerRecipe* r = &RECIPES[kind];
    size_t n = (size_t)size * (size_t)size;
    float* field = malloc(n * sizeof(float));
    unsigned char* albedo = malloc(n * 4);
    unsigned char* surface = malloc(n * 4);
    if (!field || !albedo || !surface) {
        free(field);
        free(albedo);
        free(surface);
        return;
    }

    veg_noise_seed(seed);
    relief_field(r, field, size, seed);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            size_t i = (size_t)y * (size_t)size + (size_t)x;
            float h = field[i];
            // Relief shades the base colour either side of mid, so a trough is
            // darker and a crest lighter than the authored albedo rather than
            // the whole map being one of the two.
            float lift = 1.0f + (h - 0.5f) * r->shade;
            albedo[i * 4 + 0] = to_code(r->base[0] * lift);
            albedo[i * 4 + 1] = to_code(r->base[1] * lift);
            albedo[i * 4 + 2] = to_code(r->base[2] * lift);
            // The height the blend interlocks on IS the relief. Anything else
            // would make two layers meet along a line that matches neither of
            // their surfaces.
            albedo[i * 4 + 3] = to_code(h);

            // Central differences on the wrapped lattice -- wrapped, because the
            // map tiles, and a clamped edge would leave a visible flat seam
            // exactly where the relief is supposed to continue.
            int xm = (x + size - 1) % size, xp = (x + 1) % size;
            int ym = (y + size - 1) % size, yp = (y + 1) % size;
            float dx = field[(size_t)y * size + xp] - field[(size_t)y * size + xm];
            float dz = field[(size_t)yp * size + x] - field[(size_t)ym * size + x];
            surface[i * 4 + 0] = to_code(0.5f - dx * r->relief_scale * 0.5f);
            surface[i * 4 + 1] = to_code(0.5f - dz * r->relief_scale * 0.5f);
            surface[i * 4 + 2] = to_code(r->rough_lo + (r->rough_hi - r->rough_lo) * h);
            // Troughs are occluded by what stands around them.
            surface[i * 4 + 3] = to_code(0.55f + 0.45f * h);
        }
    }

    free(field);
    if (out_albedo)
        *out_albedo = albedo;
    else
        free(albedo);
    if (out_surface)
        *out_surface = surface;
    else
        free(surface);
}
