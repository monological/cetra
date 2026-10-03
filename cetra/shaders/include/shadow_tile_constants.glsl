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
// The face's own 90 degrees as a fraction of the tile, which is 1 / tan(fov / 2) of the field
// of view that lays the face across the tile less the band each side.
#define SHADOW_TILE_INNER ((SHADOW_TILE_SIZE - 2.0f * SHADOW_TILE_GUARD) / SHADOW_TILE_SIZE)

// A cached light's near plane as a fraction of its range, where the light states none.
#define SHADOW_TILE_NEAR_RATIO 0.002f

// The views a cached light with a body is drawn from, each a cube of six faces from a point of
// the body, consecutive in its block: view 0 at the body's centre, the rest spread over it. Its
// soft shadow is their average. A light with no body is drawn from its centre alone.
#define SHADOW_TILE_VIEWS 8

// The most views --tile-reference may draw a light from: 64 views of six faces is 96 MB at
// 256 a face, within the tile budget for one light.
#define SHADOW_TILE_REFERENCE_MAX 64

// A light's body is a segment `source_length` long along its direction, centred on it, with a
// ball of `source_radius` about every point. View m of n past the first stands at u along the
// segment, stratified as (m - 0.5) / (n - 1), plus a point of the ball from the m-th triple of
// Roberts' R3 sequence, fract(0.5 + (m - 1) * alpha), alpha the reciprocal powers of the root
// of x^4 = x + 1. The kept views and the reference's are both placed this way.
#define SHADOW_TILE_R3_X 0.8191725134f
#define SHADOW_TILE_R3_Y 0.6710436067f
#define SHADOW_TILE_R3_Z 0.5497004779f

// What a cached light packs as its per-frame punctual layer: past every per-frame layer, so
// the per-frame lookup reads it as lit before it indexes anything, while every "does this light
// have a map" test (layer >= 0) still holds.
#define SHADOW_TILE_MARK 8
