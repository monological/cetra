#ifndef CETRA_PROCEDURAL_STOCHASTIC_TEX_H
#define CETRA_PROCEDURAL_STOCHASTIC_TEX_H

/*
 * By-example stochastic texturing, the bake half (Heitz & Neyret 2018).
 *
 * A tiled texture repeats, and a seamless tile only stops the SEAM -- the eye still recognises
 * the same arrangement of features arriving again, and on ground seen to the horizon it
 * arrives tens of times. The fix is to stop sampling the texture on a lattice at all: the
 * shader picks three nearby lattice cells, samples the texture at a different random offset
 * for each, and blends. Every point on the ground then shows a different part of the tile and
 * the period is gone.
 *
 * Blending three samples of a texture is normally ruinous -- it is an average, so contrast
 * collapses and the result is mush. Two things fix that, and they are the whole method:
 *
 *   The blend is VARIANCE-PRESERVING: the weighted sum is re-centred on the mean and divided
 *   by sqrt(sum of squared weights) rather than by the sum of weights, which keeps the
 *   variance of a single sample instead of averaging it away.
 *
 *   That step is only exact for a GAUSSIAN input. So the texture is transformed at bake time
 *   into one whose per-channel histogram IS Gaussian, and the original histogram is kept as a
 *   small inverse table the shader applies after blending. Sample the transform, blend it,
 *   map back -- and the result has the original texture's histogram exactly.
 *
 * WHY THIS COSTS NO SAMPLER UNIT, which is what makes it possible in pbr_frag at all: the
 * shader samples the TRANSFORMED texture and never the original, so the transform is written
 * back over the source and takes the slot it already had. The only new data is the inverse
 * table, and a few hundred floats is uniform space -- the escape the clustered-light block and
 * the shore film block already established.
 */

// Entries in the inverse table, per channel. The table is an inverse CDF, which is smooth
// except at the tails, so this is about sampling a curve rather than about preserving detail;
// 64 leaves the interpolation error well under an 8-bit code for the histograms here.
#define STOCHASTIC_LUT_SIZE 64

/*
 * Transform `rgb` (width * height * 3 bytes) in place so each channel is Gaussian, and fill
 * `inv_lut` with the inverse table that maps back.
 *
 * `inv_lut` is STOCHASTIC_LUT_SIZE * 3 floats in [0,1], INTERLEAVED -- entry (i, c) at index
 * i * 3 + c -- which is the memory layout of a vec3 array, so it uploads in one call and the
 * shader indexes it as one table rather than three.
 *
 * The stored Gaussian is mapped from [-3, 3] sigma onto [0,1] so it survives an 8-bit
 * unsigned texture; STOCHASTIC_SIGMA_SPAN is that scale and the shader undoes it.
 *
 * Safe on a NULL or degenerate input, which then leaves an identity table -- a texture that
 * failed to transform still renders, as itself, rather than as nothing.
 */
void stochastic_gaussianize(unsigned char* rgb, int width, int height, float* inv_lut);

// Half-width of the stored Gaussian range, in standard deviations. Shared with the shader,
// which must decode with the same number or every value comes back on the wrong part of the
// inverse table.
#define STOCHASTIC_SIGMA_SPAN 3.0f

#endif
