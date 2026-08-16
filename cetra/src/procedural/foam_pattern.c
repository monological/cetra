#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "foam_pattern.h"

#define FOAM_N (FOAM_PATTERN_RES * FOAM_PATTERN_RES)
// Fixed, because this texture must be byte-identical between runs or it becomes a source of
// golden drift that looks like a renderer change.
#define FOAM_SEED 0x9E3779B9u

/*
 * The three bands, as (small, large) Gaussian radii in TEXELS.
 *
 * A band-pass is one blur minus a wider one, so the pair is the band it keeps. The WEB is the
 * coarsest and is the only one folded into ridges -- it carries the filaments. The other two
 * are added flat, and their job is to stop the web reading as a regular mesh.
 */
#define FOAM_WEB_SMALL   2.0f
#define FOAM_WEB_LARGE   6.0f
#define FOAM_MID_SMALL   3.0f
#define FOAM_MID_LARGE   9.0f
#define FOAM_FINE_SMALL  1.0f
#define FOAM_FINE_LARGE  2.5f
// How hard the web is folded. Higher is a thinner, sharper strand; at 1 the ridge is as wide
// as the field and stops reading as a filament at all.
#define FOAM_RIDGE 1.2f
// The mix. The CONSTANT is load-bearing and not a brightness: it is the wash between the
// filaments, and without it the consumer's threshold cuts the web into disconnected specks
// instead of a net that thins.
#define FOAM_WEB_WEIGHT  0.55f
#define FOAM_WASH        0.25f
#define FOAM_MID_WEIGHT  0.18f
#define FOAM_FINE_WEIGHT 0.12f

static uint32_t _rand_next(uint32_t* state) {
    // Mulberry32: small, deterministic, and good enough for white noise a blur will eat.
    uint32_t z = (*state += 0x6D2B79F5u);
    z = (z ^ (z >> 15)) * (z | 1u);
    z ^= z + (z ^ (z >> 7)) * (z | 61u);
    return z ^ (z >> 14);
}

static float _rand_signed(uint32_t* state) {
    return (float)(_rand_next(state) / 4294967296.0) * 2.0f - 1.0f;
}

/*
 * Separable Gaussian blur that WRAPS.
 *
 * The wrap is not a detail: this texture tiles across the sea, so a blur that clamped at the
 * edges would leave a seam every tile -- exactly the artefact the pattern exists to hide.
 */
static void _blur_axis(const float* src, float* dst, float sigma, bool horizontal) {
    const int radius = (int)(sigma * 3.0f + 0.5f);
    float* kernel = malloc((size_t)(radius * 2 + 1) * sizeof(float));
    if (!kernel) {
        memcpy(dst, src, FOAM_N * sizeof(float));
        return;
    }
    float sum = 0.0f;
    for (int i = -radius; i <= radius; i++) {
        const float w = expf(-(float)(i * i) / (2.0f * sigma * sigma));
        kernel[i + radius] = w;
        sum += w;
    }
    for (int i = 0; i < radius * 2 + 1; i++)
        kernel[i] /= sum;

    for (int y = 0; y < FOAM_PATTERN_RES; y++) {
        for (int x = 0; x < FOAM_PATTERN_RES; x++) {
            float acc = 0.0f;
            for (int i = -radius; i <= radius; i++) {
                int sx = x, sy = y;
                if (horizontal)
                    sx = ((x + i) % FOAM_PATTERN_RES + FOAM_PATTERN_RES) % FOAM_PATTERN_RES;
                else
                    sy = ((y + i) % FOAM_PATTERN_RES + FOAM_PATTERN_RES) % FOAM_PATTERN_RES;
                acc += kernel[i + radius] * src[sy * FOAM_PATTERN_RES + sx];
            }
            dst[y * FOAM_PATTERN_RES + x] = acc;
        }
    }
    free(kernel);
}

// One band: blur at two radii and subtract, then normalise to unit variance so the weights
// above mean the same thing whatever the radii are.
static void _bandpass(const float* src, float* out, float* tmp, float sigma_small,
                      float sigma_large) {
    _blur_axis(src, tmp, sigma_small, true);
    _blur_axis(tmp, out, sigma_small, false);
    _blur_axis(src, tmp, sigma_large, true);
    float* wide = malloc(FOAM_N * sizeof(float));
    if (!wide)
        return;
    _blur_axis(tmp, wide, sigma_large, false);
    double mean = 0.0;
    for (int i = 0; i < FOAM_N; i++) {
        out[i] -= wide[i];
        mean += out[i];
    }
    mean /= FOAM_N;
    double var = 0.0;
    for (int i = 0; i < FOAM_N; i++) {
        const double d = out[i] - mean;
        var += d * d;
    }
    var = sqrt(var / FOAM_N);
    const float inv = var > 1e-9 ? (float)(1.0 / var) : 1.0f;
    for (int i = 0; i < FOAM_N; i++)
        out[i] = (float)((out[i] - mean) * inv);
    free(wide);
}

void foam_pattern_generate(float* out) {
    if (!out)
        return;
    float* white = malloc(FOAM_N * sizeof(float));
    float* tmp = malloc(FOAM_N * sizeof(float));
    float* web = malloc(FOAM_N * sizeof(float));
    float* mid = malloc(FOAM_N * sizeof(float));
    float* fine = malloc(FOAM_N * sizeof(float));
    if (!white || !tmp || !web || !mid || !fine) {
        // A flat field rather than nothing: the consumer thresholds this, and a uniform 0.5
        // degrades to "foam covers where it is strong enough", which is the pre-pattern look
        // rather than a hole in the surface.
        for (int i = 0; i < FOAM_N; i++)
            out[i] = 0.5f;
        free(white); free(tmp); free(web); free(mid); free(fine);
        return;
    }

    // One white field for all three bands, as the source they each filter a window out of.
    // Three independent fields would decorrelate the bands and the fine detail would stop
    // sitting on the web it is meant to be part of.
    uint32_t state = FOAM_SEED;
    for (int i = 0; i < FOAM_N; i++)
        white[i] = _rand_signed(&state);

    _bandpass(white, web, tmp, FOAM_WEB_SMALL, FOAM_WEB_LARGE);
    _bandpass(white, mid, tmp, FOAM_MID_SMALL, FOAM_MID_LARGE);
    _bandpass(white, fine, tmp, FOAM_FINE_SMALL, FOAM_FINE_LARGE);

    for (int i = 0; i < FOAM_N; i++) {
        // The ridge, and the whole reason this is not a noise texture.
        const float w = fmaxf(0.0f, 1.0f - FOAM_RIDGE * fabsf(web[i]));
        float v = FOAM_WEB_WEIGHT * w * w + FOAM_WASH + FOAM_MID_WEIGHT * mid[i] +
                  FOAM_FINE_WEIGHT * fine[i];
        out[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    free(white);
    free(tmp);
    free(web);
    free(mid);
    free(fine);
}
