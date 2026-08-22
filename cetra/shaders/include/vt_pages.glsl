#ifndef VT_PAGES_GLSL
#define VT_PAGES_GLSL

/*
 * The composite cache's page table (spec 11.67). C mirror: GpuVtPageBlock in
 * layers_vt_pages.h; wired to UBO_BINDING_VT_PAGES by ubo_wire_blocks after
 * link, and validated there against the C size -- which is what asserts the
 * array length below against VT_PAGE_TABLE_VEC4S, since std140 array sizes
 * cannot be shared between the two languages (the shore film's rule).
 *
 * Entries are the virtual page grid row-major, an atlas slot index or -1,
 * packed four to an ivec4 -- the iesTap idiom, because a bare int array takes
 * a vec4 stride under std140 and quadruples the block. A zero-filled buffer
 * reads pagesPerAxis 0, which every consumer treats as "no pages".
 */
layout(std140) uniform VtPageBlock {
    ivec4 vtPageInfo;  // pagesPerAxis, atlasPagesPerRow, pageTexels, gutterTexels
    vec4 vtPageParams; // atlasTexels, pageWorldSpan (usable texels' world span)
    ivec4 vtPageEntries[289];
};

int vtPageEntry(int i) {
    return vtPageEntries[i >> 2][i & 3];
}

#endif // VT_PAGES_GLSL
