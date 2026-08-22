#ifndef _LAYERS_VT_PAGES_H_
#define _LAYERS_VT_PAGES_H_

#include <stdint.h>

/*
 * The composite cache's page dimensions (spec 11.67), owned here and only laid
 * out by ubo.h -- the shore film's duplicated-constant lesson, in the same
 * shape ies.h uses.
 *
 * Pages are tiles of a single 2D atlas pair, the GI-volume layout: the gutter
 * ring is WRITTEN by the bake covering the bordered tile, never copied, and
 * bilinear/trilinear reach at the capped mip level stays inside it. Power-of-
 * two tiles aligned in a power-of-two atlas make glGenerateMipmap's 2x2 box
 * bleed-free by construction up to the cap.
 *
 * Page texel density is RELATIVE -- four times the fallback atlas's -- which is
 * what bounds the virtual grid for every domain size: the fallback resolution
 * clamps at 2048, so pages_per_axis <= ceil(4 * 2048 / usable) = 34 whether the
 * domain is forest's kilometre or --terrain-extent 4000. A fixed absolute page
 * span would overflow any table at large extents; the ratio cannot.
 */
#define VT_PAGE_TEXELS  256 // tile edge, gutter included
#define VT_PAGE_GUTTER  4   // texels; bounds filtering reach at the mip cap
#define VT_PAGE_USABLE  (VT_PAGE_TEXELS - 2 * VT_PAGE_GUTTER)
#define VT_PAGE_MIP_CAP 2 // GL_TEXTURE_MAX_LEVEL: page mip 2 meets fallback mip 0

#define VT_ATLAS_TEXELS        2048
#define VT_ATLAS_PAGES_PER_ROW (VT_ATLAS_TEXELS / VT_PAGE_TEXELS)
#define VT_PAGE_SLOTS          (VT_ATLAS_PAGES_PER_ROW * VT_ATLAS_PAGES_PER_ROW)

// The page-to-fallback density ratio. Also the handoff constant: pages beat
// the fallback exactly where the fallback is magnified, i.e. a footprint under
// one fallback texel = this many page texels.
#define VT_PAGE_DENSITY_RATIO 4

#define VT_PAGE_GRID_MAX    34
#define VT_PAGE_TABLE_MAX   (VT_PAGE_GRID_MAX * VT_PAGE_GRID_MAX)
#define VT_PAGE_TABLE_VEC4S ((VT_PAGE_TABLE_MAX + 3) / 4)

// The feedback pass encodes a vote's page coordinates one per unorm8 channel.
_Static_assert(VT_PAGE_GRID_MAX <= 255, "page coordinates must fit a vote byte");

/*
 * C mirror of VtPageBlock (vt_pages.glsl). Entries are the virtual page grid
 * row-major, each an atlas slot index or -1, packed four to an ivec4 -- the
 * iesTap idiom, because a bare int array takes a vec4 stride under std140 and
 * quadruples the block.
 */
typedef struct GpuVtPageBlock {
    // pagesPerAxis, atlasPagesPerRow, pageTexels, gutterTexels
    int32_t info[4];
    // atlasTexels, pageWorldSpan, densityRatio, unused (std140 pad)
    float params[4];
    int32_t entries[VT_PAGE_TABLE_VEC4S][4];
} GpuVtPageBlock;

#endif // _LAYERS_VT_PAGES_H_
