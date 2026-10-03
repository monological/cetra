#version 330 core

/*
 * Local exposure (spec 13.19): the frame in blocks, the blurred luminance's start. Each texel is
 * the log2 luminance of the MEAN colour of a 32x32 block of the frame -- Unreal's order, mean
 * then log, so a bright window averages in as the light it is rather than as its log.
 */

out float Block;

uniform sampler2D halfTex; // .rgb: the mean colour of each 2x2 block of the frame

// Half-res texels a block spans each way: 32 frame pixels. local_exposure.c's LE_BLOCK.
const int LE_BLOCK = 16;

void main() {
    ivec2 size = textureSize(halfTex, 0);
    ivec2 lo = ivec2(gl_FragCoord.xy) * LE_BLOCK;
    ivec2 hi = min(lo + LE_BLOCK, size);
    vec3 c = vec3(0.0);
    for (int y = lo.y; y < hi.y; y++)
        for (int x = lo.x; x < hi.x; x++)
            c += texelFetch(halfTex, ivec2(x, y), 0).rgb;
    ivec2 n = max(hi - lo, ivec2(1));
    c /= float(n.x * n.y);
    Block = log2(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1.0e-8));
}
