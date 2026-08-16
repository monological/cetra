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
/*
 * Taps in the widest kernel any radius above needs, which is what lets the blur keep its kernel
 * on the stack instead of allocating one it could fail to get.
 *
 * _blur_axis takes radius = sigma * 3 + 0.5 and 2 * radius + 1 taps, so the largest sigma here
 * decides it. The assert is against that constant rather than against a copied number, so
 * widening a band cannot outgrow the buffer silently.
 */
#define FOAM_BLUR_MAX_TAPS 64
_Static_assert(2 * (int)(FOAM_MID_LARGE * 3.0f + 0.5f) + 1 <= FOAM_BLUR_MAX_TAPS,
               "the widest band's kernel no longer fits the blur's stack buffer");
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
    // On the stack: the radii are compile-time constants of this file, so the largest kernel is
    // known and the allocation that used to be here could only fail by silently returning the
    // field UNBLURRED -- which the caller then band-passed against itself.
    float kernel[FOAM_BLUR_MAX_TAPS];
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
}

/*
 * One band: blur at two radii and subtract, then normalise to unit variance so the weights
 * above mean the same thing whatever the radii are.
 *
 * Both scratch buffers come from the caller. `wide` used to be malloc'd here and the failure
 * path returned early, which left `out` holding the small-radius blur alone -- not band-passed,
 * not mean-subtracted, not normalised -- and the caller then weighted it as though it were the
 * unit-variance band this promises. A silently wrong texture rather than a degraded one, and
 * indistinguishable from success. With the allocation hoisted, this cannot half-fail.
 */
static void _bandpass(const float* src, float* out, float* tmp, float* wide, float sigma_small,
                      float sigma_large) {
    _blur_axis(src, tmp, sigma_small, true);
    _blur_axis(tmp, out, sigma_small, false);
    _blur_axis(src, tmp, sigma_large, true);
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
}

bool foam_pattern_generate(float* out) {
    if (!out)
        return false;
    /*
     * ONE ARENA for all six scratch fields, which is what makes the failure path a single
     * question with a single answer.
     *
     * There were six allocation sites and three different degradations -- an unblurred field
     * passed off as blurred, a band-pass that returned half-done, and a flat 0.5 -- so "the
     * bake failed" could mean any of three things and one of them was silent. Every buffer is
     * the same compile-time size, so one block covers all of them and the only outcome left is
     * the honest one: it worked, or it did not and the caller is told.
     */
    const size_t stride = FOAM_N;
    float* arena = malloc(6 * stride * sizeof(float));
    if (!arena)
        return false;
    float* white = arena;
    float* tmp = arena + stride;
    float* wide = arena + 2 * stride;
    float* web = arena + 3 * stride;
    float* mid = arena + 4 * stride;
    float* fine = arena + 5 * stride;

    // One white field for all three bands, as the source they each filter a window out of.
    // Three independent fields would decorrelate the bands and the fine detail would stop
    // sitting on the web it is meant to be part of.
    uint32_t state = FOAM_SEED;
    for (int i = 0; i < FOAM_N; i++)
        white[i] = _rand_signed(&state);

    _bandpass(white, web, tmp, wide, FOAM_WEB_SMALL, FOAM_WEB_LARGE);
    _bandpass(white, mid, tmp, wide, FOAM_MID_SMALL, FOAM_MID_LARGE);
    _bandpass(white, fine, tmp, wide, FOAM_FINE_SMALL, FOAM_FINE_LARGE);

    for (int i = 0; i < FOAM_N; i++) {
        // The ridge, and the whole reason this is not a noise texture.
        const float w = fmaxf(0.0f, 1.0f - FOAM_RIDGE * fabsf(web[i]));
        float v = FOAM_WEB_WEIGHT * w * w + FOAM_WASH + FOAM_MID_WEIGHT * mid[i] +
                  FOAM_FINE_WEIGHT * fine[i];
        out[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    free(arena);
    return true;
}
