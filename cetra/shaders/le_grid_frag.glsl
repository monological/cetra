#version 330 core

/*
 * Local exposure (spec 13.19): the bilateral grid, one (cell, bin) a fragment, as the sum of the
 * cell's sub-blocks from le_bins_frag.glsl. Normalised by the cell's area, so a cell the frame's
 * edge cuts short weighs only what it holds.
 */

out vec2 Cell;

uniform sampler2D partTex; // each sub-block's bins, leBinShape() texels a sub-block

#include "le_grid.glsl"

// Half-res texels a cell and a sub-block span each way. local_exposure.c's LE_CELL and LE_PART.
const int LE_CELL = 64;
const int LE_PART = 8;
const int LE_PARTS = LE_CELL / LE_PART;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 tile = p / leGrid.xy;
    int bin = tile.y * leGrid.w + tile.x;
    if (bin >= leGrid.z) {
        Cell = vec2(0.0);
        return;
    }
    ivec2 cell = p - tile * leGrid.xy;
    ivec2 shape = leBinShape();
    ivec2 k = ivec2(bin % shape.x, bin / shape.x);
    vec2 acc = vec2(0.0);
    for (int y = 0; y < LE_PARTS; y++)
        for (int x = 0; x < LE_PARTS; x++)
            acc += texelFetch(partTex, (cell * LE_PARTS + ivec2(x, y)) * shape + k, 0).rg;
    Cell = acc / float(LE_CELL * LE_CELL);
}
