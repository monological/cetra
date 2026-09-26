/*
 * The ripple band's transform size (spec 13.3), INCLUDED BY BOTH LANGUAGES: water.c allocates
 * the band and publishes one slope variance per mip level from these; ocean.glsl declares that
 * array and turns a footprint into a level. An array length spelled once per language is a
 * uniform upload that silently stops short, or runs past the end.
 *
 * Numbers only, for shore_constants.glsl's reasons: `#define`s, no type, function or qualifier.
 * Integers, which mean the same thing to both preprocessors.
 *
 * Its own size rather than the cascades' 128: the band exists to carry centimetre waves, and a
 * 128 over any tile short enough for that repeats every metre or two, which is a visible print.
 * 512 over 6 m is a 1.17 cm texel -- an eighth of the short band's, so every short-band texel
 * is a whole number of these and the two stay commensurate.
 */
#define WATER_RIPPLE_RES  512
#define WATER_RIPPLE_LOG  9  // log2(WATER_RIPPLE_RES)
#define WATER_RIPPLE_LODS 10 // levels in the mip chain, WATER_RIPPLE_LOG + 1
