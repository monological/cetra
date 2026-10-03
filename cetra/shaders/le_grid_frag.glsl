#version 330 core

/*
 * Local exposure (spec 13.19): the bilateral grid, built by GATHER. GL 4.1 has no compute and no
 * atomics, so where Unreal splats each texel into its cell, here each (cell, bin) fragment walks
 * the cell's half-res texels and keeps the share of each that falls in its bin -- the meter's
 * histogram is built the same way and for the same reason. The loop order is fixed, so the sums
 * are the same on every run.
 *
 * A texel is split between its two nearest bins by a tent, as Unreal splits it, rather than
 * rounded to one as Chen et al. do: a surface whose luminance crosses a bin boundary then moves
 * its weight across smoothly instead of all at once. Stored homogeneous, (sum of w * log2
 * luminance, sum of w) with w normalised by the cell's area, which is what lets the blur after
 * this and the tonemap's interpolation average cells of different populations correctly.
 */

out vec2 Cell;

uniform sampler2D halfTex; // .a: log2 luminance at half resolution

#include "le_grid.glsl"

// Half-res texels a cell spans each way (Unreal's 64). local_exposure.c's LE_CELL.
const int LE_CELL = 64;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 tile = p / leGrid.xy;
    int bin = tile.y * leGrid.w + tile.x;
    if (bin >= leGrid.z) {
        Cell = vec2(0.0);
        return;
    }
    ivec2 cell = p - tile * leGrid.xy;
    ivec2 size = textureSize(halfTex, 0);
    ivec2 lo = cell * LE_CELL;
    ivec2 hi = min(lo + LE_CELL, size);
    vec2 acc = vec2(0.0);
    for (int y = lo.y; y < hi.y; y++) {
        for (int x = lo.x; x < hi.x; x++) {
            float l = texelFetch(halfTex, ivec2(x, y), 0).a;
            // The edge bins take everything past them, so a pixel outside the grid's span still
            // has a base: its nearest bin's.
            float z = clamp(leBinCoord(l), 0.0, float(leGrid.z - 1));
            float w = max(0.0, 1.0 - abs(z - float(bin)));
            acc += w * vec2(l, 1.0);
        }
    }
    Cell = acc / float(LE_CELL * LE_CELL);
}
