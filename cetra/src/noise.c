#include "noise.h"

#include <math.h>
#include <stdlib.h>

// Permutation table (Ken Perlin's improved-noise scheme), doubled to 512 so the
// gradient index math never wraps. Promoted from the app-local 2D version in
// apps/tree with a 3D gradient + curl added.
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

float noise_fbm3(float x, float y, float z, int octaves, float persistence) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;
    for (int i = 0; i < octaves; i++) {
        total += noise_perlin3(x * frequency, y * frequency, z * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }
    return max_value > 0.0f ? total / max_value : 0.0f;
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
