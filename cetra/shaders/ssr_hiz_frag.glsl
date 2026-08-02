#version 330 core
out vec4 FragColor;

// One level of the SSR min-depth pyramid: each output texel is the MINIMUM
// (nearest) depth of the 2x2 source texels below it, so any level
// conservatively bounds the nearest surface over its footprint — a ray
// traversal against the pyramid cannot step over thin geometry. When the
// source dimension is odd, the last row/column folds into the edge texels
// (the extra taps keep the bound conservative there too).
uniform sampler2D srcTex; // full-res depth (level 0) or the previous level
uniform int srcWidth;
uniform int srcHeight;
// Full-res tracing builds level 0 at the same size as the depth buffer, so it
// is a straight 1:1 copy (min over one texel = the depth itself), not the 2:1
// reduction every other level does. Half-res level 0 and all deeper levels
// leave this 0 and reduce 2x2. The 1:1 identity is load-bearing: a full-res
// level-0 cell may be read downstream as the exact scene depth of one screen
// column, not merely a conservative bound.
uniform int copySrc;

float srcAt(ivec2 p)
{
    return texelFetch(srcTex, min(p, ivec2(srcWidth, srcHeight) - 1), 0).r;
}

void main()
{
    if (copySrc != 0) {
        FragColor = vec4(srcAt(ivec2(gl_FragCoord.xy)), 0.0, 0.0, 1.0);
        return;
    }
    ivec2 s = ivec2(gl_FragCoord.xy) * 2;

    float m = min(min(srcAt(s), srcAt(s + ivec2(1, 0))),
                  min(srcAt(s + ivec2(0, 1)), srcAt(s + ivec2(1, 1))));

    bool odd_x = (srcWidth & 1) != 0;
    bool odd_y = (srcHeight & 1) != 0;
    if (odd_x) {
        m = min(m, min(srcAt(s + ivec2(2, 0)), srcAt(s + ivec2(2, 1))));
    }
    if (odd_y) {
        m = min(m, min(srcAt(s + ivec2(0, 2)), srcAt(s + ivec2(1, 2))));
    }
    if (odd_x && odd_y) {
        m = min(m, srcAt(s + ivec2(2, 2)));
    }

    FragColor = vec4(m, 0.0, 0.0, 1.0);
}
