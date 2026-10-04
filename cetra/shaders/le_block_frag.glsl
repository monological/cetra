#version 330 core

/*
 * Local exposure (spec 13.19): the frame in blocks, the blurred luminance's start. Each texel is
 * the log2 luminance of the MEAN colour of a 32x32 block of the frame -- Unreal's order, mean
 * then log, so a bright window averages in as the light it is rather than as its log. Summed from
 * le_block_sum_frag.glsl's 4x4s.
 */

out float Block;

uniform sampler2D sumTex; // .rgb: summed colour, .a: how many half-res texels it sums

// Texels a stage sums each way; two stages make a block. local_exposure.c's LE_BLOCK_STEP.
const int LE_BLOCK_STEP = 4;

void main() {
    ivec2 size = textureSize(sumTex, 0);
    ivec2 lo = ivec2(gl_FragCoord.xy) * LE_BLOCK_STEP;
    ivec2 hi = min(lo + LE_BLOCK_STEP, size);
    vec4 s = vec4(0.0);
    for (int y = lo.y; y < hi.y; y++)
        for (int x = lo.x; x < hi.x; x++)
            s += texelFetch(sumTex, ivec2(x, y), 0);
    vec3 c = s.rgb / s.a;
    Block = log2(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1.0e-8));
}
