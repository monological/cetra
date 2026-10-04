#version 330 core

/*
 * Local exposure (spec 13.19): the blurred luminance's blocks, first half. Each texel sums a 4x4
 * of the half-res frame's colour and counts what it summed, so le_block_frag.glsl's mean over a
 * block the frame's edge cuts short still weighs every texel once. Two passes of sixteen taps
 * and not one of 256, for the reason the grid is gathered in two: a few thousand long serial
 * loops cost 0.7 ms at 4K.
 */

out vec4 Sum;

uniform sampler2D halfTex; // .rgb: the mean colour of each 2x2 block of the frame

// Texels a stage sums each way; two stages make a block. local_exposure.c's LE_BLOCK_STEP.
const int LE_BLOCK_STEP = 4;

void main() {
    ivec2 size = textureSize(halfTex, 0);
    ivec2 lo = ivec2(gl_FragCoord.xy) * LE_BLOCK_STEP;
    ivec2 hi = min(lo + LE_BLOCK_STEP, size);
    vec3 c = vec3(0.0);
    for (int y = lo.y; y < hi.y; y++)
        for (int x = lo.x; x < hi.x; x++)
            c += texelFetch(halfTex, ivec2(x, y), 0).rgb;
    ivec2 n = hi - lo;
    Sum = vec4(c, float(n.x * n.y));
}
