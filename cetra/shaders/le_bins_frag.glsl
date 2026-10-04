#version 330 core

/*
 * Local exposure (spec 13.19): the bilateral grid's first half, built by GATHER. GL 4.1 has no
 * compute and no atomics, so where Unreal splats each texel into its cell, here a fragment owns
 * one (sub-block, bin) pair and walks the sub-block's half-res texels, keeping the share of each
 * that falls in its bin; le_grid_frag.glsl then sums a cell's sub-blocks. The meter's histogram
 * is built the same way and for the same reason. The loop order is fixed, so the sums are the
 * same on every run.
 *
 * Two passes and not one, for the histogram's reason: one fragment per (cell, bin) walking all
 * 4096 of a cell's texels is a few thousand long serial loops, and at a retina frame's size it
 * cost 18.8 ms. The bins of a sub-block are a block of neighbouring fragments, so the fragments
 * fetching the same texels run together.
 *
 * A texel is split between its two nearest bins by a tent, as Unreal splits it, rather than
 * rounded to one as Chen et al. do: a surface whose luminance crosses a bin boundary then moves
 * its weight across smoothly instead of all at once. Stored homogeneous, (sum of w * log2
 * luminance, sum of w), which is what lets the blur after this and the tonemap's interpolation
 * average cells of different populations correctly.
 */

out vec2 Part;

uniform sampler2D halfTex; // .a: log2 luminance at half resolution

#include "le_grid.glsl"

// Half-res texels a sub-block spans each way. local_exposure.c's LE_PART.
const int LE_PART = 8;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 shape = leBinShape();
    ivec2 part = p / shape;
    ivec2 k = p - part * shape;
    int bin = k.y * shape.x + k.x;
    ivec2 size = textureSize(halfTex, 0);
    ivec2 lo = part * LE_PART;
    ivec2 hi = min(lo + LE_PART, size);
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
    Part = acc;
}
