#ifndef _MOON_SURFACE_H_
#define _MOON_SURFACE_H_

/*
 * The lunar surface, baked once into one equirectangular image.
 *
 * WHY A BAKE AND NOT A SHADER FIELD. The first version evaluated a crater
 * lattice per pixel, which caps the crater population at what a 3x3x3
 * neighbourhood can hold -- about 27 candidates per octave, on a grid. A crater
 * field is not sparse and is not on a grid: it is billions of years of
 * overprinting, and what the eye reads is the OVERLAP, rims cutting through
 * older rims. Reaching that per pixel would cost a neighbourhood nobody can
 * afford. Baked, the population is free: this stamps ~45,000 craters across
 * three size tiers, and the per-pixel cost falls to one texture fetch.
 *
 * WHY IT IS GENERATED AND NOT DOWNLOADED. Same reason moon_map.h is generated --
 * no asset, no licence, no binary in the tree, and it is reproducible. The
 * maria layout is the one thing that cannot be synthesised (see gen_moon_map.py)
 * and arrives as data; everything here is built on top of it.
 *
 * The result is RGBA8, `w` by `h`, equirectangular in the same frame moon_map.h
 * uses -- row 0 at +90 latitude, column 0 at -180 longitude, positive east:
 *
 *   RGB  the surface NORMAL in the local TANGENT frame (east, north, up),
 *        0.5-biased, exactly as any other normal map
 *   A    albedo, 0 black to 1 the brightest fresh ejecta
 *
 * Normals rather than a height channel because 8 bits of height differentiates
 * into visible terracing, and because the consumer wants the normal anyway --
 * baking it spends the float precision where it exists and leaves the shader
 * nothing to reconstruct. TANGENT frame rather than body frame because that is
 * what makes the mip chain correct: a tangent normal averages toward flat, so a
 * moon drawn a few pixels across reads as a smooth sphere, where a body-frame
 * normal averages toward the mean of the sphere's own normals -- zero, and
 * normalising zero is how a distant moon turns into noise. Albedo is a separate channel and not
 * folded into the normal's shading because the two are genuinely independent: relief is invisible
 * at full phase and albedo is all there is, which is the whole reason a moon rendered from relief
 * alone reads as a blank disc.
 *
 * Deterministic, and bit-identical at any worker count: bands are disjoint rows,
 * and every texel sees the same craters in the same order whatever the split.
 */

// The shipping size. 2048 texels around the equator puts one at 0.0031 rad,
// which is what sets the crater size floor in the .c; and since the near side
// spans half the width, a disc drawn at a thousand pixels sits near 1:1.
#define MOON_SURFACE_W 2048
#define MOON_SURFACE_H 1024

// Returns a malloc'd w*h*4 buffer the caller owns, or NULL on allocation
// failure. `workers` of 0 sizes from the machine.
unsigned char* moon_surface_bake(int w, int h, int workers);

#endif // _MOON_SURFACE_H_
