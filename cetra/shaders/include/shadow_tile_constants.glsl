/*
 * Cached point-light shadows' numbers that the CPU and the GPU must agree on (spec 13.16).
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique and for its reason: shadow.c
 * draws each cached face into a tile of the punctual array, and the lookup projects onto that
 * face and finds that tile with the same numbers. A tile size or a guard band the two disagreed
 * on would read another face's depth, or another light's. Numbers only, `f`-suffixed; no
 * types, functions or qualifiers.
 */

// A cached face's tile, in texels, and the guard band inside it: the face is drawn across 90
// degrees plus the band, so a filter reaching past the face's edge still reads this face's
// depth and never a neighbouring tile's.
#define SHADOW_TILE_SIZE  256
#define SHADOW_TILE_GUARD 8

// A cached light's near plane as a fraction of its range, where the light states none.
#define SHADOW_TILE_NEAR_RATIO 0.002f

// What a cached light packs as its per-frame punctual layer: past every per-frame layer, so
// the per-frame lookup reads it as lit before it indexes anything, while every "does this light
// have a map" test (layer >= 0) still holds.
#define SHADOW_TILE_MARK 8
