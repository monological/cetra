#version 330 core

/*
 * Local exposure (spec 13.19): the grid's processing, Chen, Paris & Durand's 3D Gaussian, one
 * axis a pass, five taps, sigma one cell or one bin -- they set the sampling rates to the
 * sigmas. Homogeneous values blur linearly, so a cell with few pixels contributes little and the
 * division at the slice still gives the weighted mean. Past the grid is ZERO, not the edge cell:
 * there is nothing there, and repeating the edge would weigh it twice.
 */

out vec2 Cell;

uniform sampler2D gridTex;
uniform int leAxis; // 0 across, 1 down, 2 along the bins

#include "le_grid.glsl"

const float LE_TAPS[3] = float[3](1.0, 0.60653066, 0.13533528); // exp(-d^2 / 2)
const float LE_TAPS_SUM = 1.0 + 2.0 * (0.60653066 + 0.13533528);

vec2 leCellAt(ivec3 c) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, leGrid.xyz)))
        return vec2(0.0);
    return texelFetch(gridTex, leTileOrigin(c.z) + c.xy, 0).rg;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 tile = p / leGrid.xy;
    int bin = tile.y * leGrid.w + tile.x;
    if (bin >= leGrid.z) {
        Cell = vec2(0.0);
        return;
    }
    ivec3 c = ivec3(p - tile * leGrid.xy, bin);
    ivec3 step = leAxis == 0 ? ivec3(1, 0, 0) : (leAxis == 1 ? ivec3(0, 1, 0) : ivec3(0, 0, 1));
    vec2 acc = LE_TAPS[0] * leCellAt(c);
    for (int d = 1; d <= 2; d++)
        acc += LE_TAPS[d] * (leCellAt(c + d * step) + leCellAt(c - d * step));
    Cell = acc / LE_TAPS_SUM;
}
