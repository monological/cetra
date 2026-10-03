#version 330 core

/*
 * Local exposure (spec 13.19): the blurred luminance, one axis a pass. Unreal's Gaussian from
 * PostProcessWeightedSampleSum, exp(-16.7 (x/r)^2) out to r texels either side, with the frame's
 * edges mirrored -- a wide blur clamped at the edge would pull every border toward its own last
 * texel. Its result is written into the atlas's blurred region on the second pass, so
 * gl_FragCoord is offset by where that region starts.
 */

out vec4 Blurred;

uniform sampler2D blockTex; // .r: log2 luminance
uniform ivec4 leStep;       // the axis, (1, 0) or (0, 1), then where the target region starts
uniform int leRadius;       // r, in texels

void main() {
    ivec2 size = textureSize(blockTex, 0);
    ivec2 leDir = leStep.xy;
    ivec2 p = ivec2(gl_FragCoord.xy) - leStep.zw;
    float r = float(max(leRadius, 1));
    float sum = 0.0, wsum = 0.0;
    for (int d = -leRadius; d <= leRadius; d++) {
        ivec2 q = p + d * leDir;
        // Mirrored about the edge texel's outer side: -1 reads 0, size reads size - 1.
        q = ivec2(q.x < 0 ? -q.x - 1 : q.x, q.y < 0 ? -q.y - 1 : q.y);
        q = ivec2(q.x >= size.x ? 2 * size.x - 1 - q.x : q.x,
                  q.y >= size.y ? 2 * size.y - 1 - q.y : q.y);
        float x = float(d) / r;
        float w = exp(-16.7 * x * x);
        sum += w * texelFetch(blockTex, clamp(q, ivec2(0), size - 1), 0).r;
        wsum += w;
    }
    Blurred = vec4(sum / wsum, 0.0, 0.0, 0.0);
}
