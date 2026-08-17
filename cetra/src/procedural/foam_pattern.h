#ifndef CETRA_FOAM_PATTERN_H
#define CETRA_FOAM_PATTERN_H

#include <stdbool.h>

/*
 * THE FOAM PATTERN: a tiling web of filaments, baked once (spec 11.45).
 *
 * Whitewater is not blobs. It is a web -- thin films stretched between bubbles -- so the
 * structure that matters is filaments and the cells between them, and a smooth noise cannot
 * produce that however it is thresholded: its level sets are regions, and what foam needs is
 * curves.
 *
 * Folding a zero-mean field about zero is what makes curves out of a field. `1 - |x|` puts a
 * crest along every zero crossing, which is a curve by construction, and squaring it sharpens
 * that crest into a strand.
 *
 * BAKED rather than evaluated per fragment, and the mip chain is the reason. The shader had a
 * procedural version of this and it had no mips, so the far field lost its filtering and was
 * protected only by foam fading out before it aliased. Whitewater is centimetre-scale detail
 * seen at every distance from a metre to the horizon, which is precisely the case mips exist
 * for.
 *
 * Deterministic from a fixed seed: two runs produce identical texels, so this cannot become a
 * source of golden drift.
 */

// Side of the generated texture, and the world span one tile covers, in METRES.
//
// Was 5 m. Measured against apps/tree's actual sea state (spec 11.47's review-fix pass):
// individual crest-foam selections there run 5-10 real metres across, so a 5 m tile fit
// zero to one period inside a whole selection -- there was no room left for the pattern to
// TILE, which is the only mechanism that fragments a solid selection into filaments. The
// erosion PASS RATE is unaffected by this constant (it depends on the baked texture's own
// value distribution, sampled at a different frequency, not a different distribution), so
// this changes granularity, not how much foam survives on average.
//
// 1.5 m over 256 texels is 6 mm a texel, still a clump of bubbles rather than a single one,
// and gives at least three tile periods across even the smallest selections measured above.
#define FOAM_PATTERN_RES    256
#define FOAM_PATTERN_TILE_M 1.5f

/*
 * Fill `out` (FOAM_PATTERN_RES^2 floats, 0..1) with the pattern's density.
 *
 * One channel: the consumer thresholds it against a bar that rises as foam thins, so what is
 * wanted here is a height field to cut, not a colour.
 *
 * Returns false if the scratch allocation failed, in which case `out` is untouched. It reports
 * rather than degrading because a partly-built field here is indistinguishable from a built one
 * downstream -- the consumer would threshold against it and the frame would be quietly wrong.
 * The caller's answer is to tell the shader there is no pattern, which is a real code path.
 */
bool foam_pattern_generate(float* out);

#endif // CETRA_FOAM_PATTERN_H
