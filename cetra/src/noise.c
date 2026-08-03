#include "noise.h"

#include <math.h>
#include <stdlib.h>

// Permutation table (Ken Perlin's improved-noise scheme), doubled to 512 so the
// gradient index math never wraps. Uses the same Fisher-Yates build as the
// separate 2D Perlin in apps/tree, with a 3D gradient + curl added here.
static int s_perm[512];
static int s_seeded = 0;

void noise_seed(unsigned int seed) {
    srand(seed);
    int p[256];
    for (int i = 0; i < 256; i++)
        p[i] = i;
    for (int i = 255; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
    for (int i = 0; i < 512; i++)
        s_perm[i] = p[i & 255];
    s_seeded = 1;
}

static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerp_f(float a, float b, float t) {
    return a + t * (b - a);
}

static float grad3(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

float noise_perlin3(float x, float y, float z) {
    if (!s_seeded)
        noise_seed(12345);

    int X = (int)floorf(x) & 255;
    int Y = (int)floorf(y) & 255;
    int Z = (int)floorf(z) & 255;
    float xf = x - floorf(x);
    float yf = y - floorf(y);
    float zf = z - floorf(z);

    float u = fade(xf);
    float v = fade(yf);
    float w = fade(zf);

    int A = s_perm[X] + Y, AA = s_perm[A] + Z, AB = s_perm[A + 1] + Z;
    int B = s_perm[X + 1] + Y, BA = s_perm[B] + Z, BB = s_perm[B + 1] + Z;

    return lerp_f(
        lerp_f(lerp_f(grad3(s_perm[AA], xf, yf, zf), grad3(s_perm[BA], xf - 1, yf, zf), u),
               lerp_f(grad3(s_perm[AB], xf, yf - 1, zf), grad3(s_perm[BB], xf - 1, yf - 1, zf), u),
               v),
        lerp_f(lerp_f(grad3(s_perm[AA + 1], xf, yf, zf - 1),
                      grad3(s_perm[BA + 1], xf - 1, yf, zf - 1), u),
               lerp_f(grad3(s_perm[AB + 1], xf, yf - 1, zf - 1),
                      grad3(s_perm[BB + 1], xf - 1, yf - 1, zf - 1), u),
               v),
        w);
}

void noise_perm_init(NoisePerm* t, unsigned int seed) {
    unsigned int state = seed ? seed : 1u;
    int p[256];
    for (int i = 0; i < 256; i++)
        p[i] = i;
    for (int i = 255; i > 0; i--) {
        state = state * 1664525u + 1013904223u;
        int j = (int)(state % (unsigned int)(i + 1));
        int tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
    for (int i = 0; i < 512; i++)
        t->p[i] = p[i & 255];
}

// Lattice hash for the tiled variant: nested lookups on WRAPPED corner
// coordinates, so the field repeats exactly every `period` cells. The
// global-table Perlin above uses additive indices instead -- the two fields
// are deliberately unrelated.
static int tiled_hash(const NoisePerm* t, int x, int y, int z) {
    return t->p[(t->p[(t->p[x & 255] + y) & 255] + z) & 255];
}

float noise_perlin3_tiled(const NoisePerm* t, float x, float y, float z, int period) {
    int X = (int)floorf(x);
    int Y = (int)floorf(y);
    int Z = (int)floorf(z);
    float xf = x - floorf(x);
    float yf = y - floorf(y);
    float zf = z - floorf(z);

    float u = fade(xf);
    float v = fade(yf);
    float w = fade(zf);

    int x0 = ((X % period) + period) % period;
    int y0 = ((Y % period) + period) % period;
    int z0 = ((Z % period) + period) % period;
    int x1 = (x0 + 1) % period;
    int y1 = (y0 + 1) % period;
    int z1 = (z0 + 1) % period;

    return lerp_f(
        lerp_f(lerp_f(grad3(tiled_hash(t, x0, y0, z0), xf, yf, zf),
                      grad3(tiled_hash(t, x1, y0, z0), xf - 1, yf, zf), u),
               lerp_f(grad3(tiled_hash(t, x0, y1, z0), xf, yf - 1, zf),
                      grad3(tiled_hash(t, x1, y1, z0), xf - 1, yf - 1, zf), u),
               v),
        lerp_f(lerp_f(grad3(tiled_hash(t, x0, y0, z1), xf, yf, zf - 1),
                      grad3(tiled_hash(t, x1, y0, z1), xf - 1, yf, zf - 1), u),
               lerp_f(grad3(tiled_hash(t, x0, y1, z1), xf, yf - 1, zf - 1),
                      grad3(tiled_hash(t, x1, y1, z1), xf - 1, yf - 1, zf - 1), u),
               v),
        w);
}

float noise_worley3(float x, float y, float z, int period, unsigned int seed) {
    int xi = (int)floorf(x);
    int yi = (int)floorf(y);
    int zi = (int)floorf(z);

    float min_dist = 999.0f;

    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = xi + dx;
                int cy = yi + dy;
                int cz = zi + dz;

                // The feature point's offset hashes from the WRAPPED cell id
                // (so opposite faces agree -- the tiling), but its position
                // anchors to the UNwrapped cell (so distances are measured in
                // the sample's own frame).
                int wx = ((cx % period) + period) % period;
                int wy = ((cy % period) + period) % period;
                int wz = ((cz % period) + period) % period;
                unsigned int h = (unsigned int)wx * 374761393u + (unsigned int)wy * 668265263u +
                                 (unsigned int)wz * 2654435761u + seed;
                h = (h ^ (h >> 13)) * 1274126177u;
                h ^= h >> 16;

                float fx = (float)cx + (float)(h & 0x3FF) / 1024.0f;
                float fy = (float)cy + (float)((h >> 10) & 0x3FF) / 1024.0f;
                float fz = (float)cz + (float)((h >> 20) & 0x3FF) / 1024.0f;

                float d = (x - fx) * (x - fx) + (y - fy) * (y - fy) + (z - fz) * (z - fz);
                if (d < min_dist)
                    min_dist = d;
            }
        }
    }

    return sqrtf(min_dist);
}

void noise_curl3(float x, float y, float z, vec3 out) {
    const float e = 0.1f; // finite-difference epsilon
    const float inv = 1.0f / (2.0f * e);

    // Three decorrelated scalar potentials (offset the domain per component).
#define PX(a, b, c) noise_perlin3((a), (b), (c))
#define PY(a, b, c) noise_perlin3((a) + 31.416f, (b) - 47.853f, (c) + 12.793f)
#define PZ(a, b, c) noise_perlin3((a) - 19.264f, (b) + 33.148f, (c) - 8.421f)

    float dPz_dy = (PZ(x, y + e, z) - PZ(x, y - e, z)) * inv;
    float dPy_dz = (PY(x, y, z + e) - PY(x, y, z - e)) * inv;
    float dPx_dz = (PX(x, y, z + e) - PX(x, y, z - e)) * inv;
    float dPz_dx = (PZ(x + e, y, z) - PZ(x - e, y, z)) * inv;
    float dPy_dx = (PY(x + e, y, z) - PY(x - e, y, z)) * inv;
    float dPx_dy = (PX(x, y + e, z) - PX(x, y - e, z)) * inv;

#undef PX
#undef PY
#undef PZ

    out[0] = dPz_dy - dPy_dz;
    out[1] = dPx_dz - dPz_dx;
    out[2] = dPy_dx - dPx_dy;
}
