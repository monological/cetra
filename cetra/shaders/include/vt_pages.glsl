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
    vec4 vtPageParams; // atlasTexels, pageWorldSpan, densityRatio, unused (std140 pad)
    ivec4 vtPageEntries[289];
};

int vtPageEntry(int i) {
    return vtPageEntries[i >> 2][i & 3];
}

// The uv-to-page mapping, in the ONE place both consumers share: the shading
// read and the feedback vote must name the same page from the same position,
// or feedback boosts pages the read never samples -- and no arm attributes
// that drift quickly. True when uv lands on a valid page; pf carries the
// fractional page coordinate for the read side's atlas transform. Residency is
// deliberately NOT part of this test -- a vote for a non-resident page is the
// demand signal feedback exists to deliver.
bool vtPageCoord(vec2 uv, vec2 domainSize, out vec2 pf, out ivec2 pi) {
    pf = vec2(0.0);
    pi = ivec2(0);
    if (vtPageInfo.x <= 0)
        return false;
    pf = uv * domainSize / vtPageParams.y;
    pi = ivec2(floor(pf));
    return pi.x >= 0 && pi.y >= 0 && pi.x < vtPageInfo.x && pi.y < vtPageInfo.x;
}

// The read side's composition: the atlas slot for uv, or -1 when there is no
// grid, the coordinate is out of bounds, or the page is not resident.
int vtPageSlot(vec2 uv, vec2 domainSize, out vec2 pf) {
    ivec2 pi;
    if (!vtPageCoord(uv, domainSize, pf, pi))
        return -1;
    return vtPageEntry(pi.y * vtPageInfo.x + pi.x);
}

#endif // VT_PAGES_GLSL
