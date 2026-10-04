// Local exposure's bilateral grid (spec 13.19; Chen, Paris & Durand 2007) as a flat 2D atlas:
// bin k is the tile at (k % tilesX, k / tilesX), cells.x by cells.y texels, each texel a
// homogeneous (sum of log2 luminance, weight). The blurred luminance has a region of its own in
// the same atlas, so the tonemap reads the whole thing through one sampler. local_exposure.c
// owns every number here and uploads them.
//
// Cell (i, j)'s centre is at grid coordinate (i, j) + 0.5, and bin k's centre at log2 luminance
// leRange.x + (k + 0.5) * leRange.y.

uniform ivec4 leGrid;     // cells across, cells down, bins, tiles across
uniform vec2 leRange;     // log2 luminance where bin 0's span starts, stops a bin
uniform ivec4 leBlurRect; // the blurred luminance's region in the atlas: x, y, width, height
uniform vec2 leCellScale; // grid cells per unit of the frame's uv, each way

ivec2 leTileOrigin(int bin) {
    return ivec2((bin % leGrid.w) * leGrid.x, (bin / leGrid.w) * leGrid.y);
}

// The bins laid out as the tiles are, as one block: how le_bins_frag.glsl places one
// sub-block's bins side by side.
ivec2 leBinShape() {
    return ivec2(leGrid.w, leGrid.z / leGrid.w);
}

// Where log2 luminance `y` falls among the bins, continuously: bin k's centre is at k.
float leBinCoord(float y) {
    return (y - leRange.x) / leRange.y - 0.5;
}
