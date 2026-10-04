#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// The SSR fold for a frame with wet ground (spec 13.9), in place of upsample_tent_frag's plain
// tent, under the same (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) blend.
//
// Wet ground takes nothing here. Its pairs replace a share of the environment's reflection
// rather than lerping toward the trace (see ssr_frag), and the next frame's split composite folds
// them in before TAA, beside that share (spec 13.21). Only the catcher folds here, and the tent
// averages it only with its own class: a wet pair's bare coverage taken as a catcher's Fresnel
// would lerp the floor beside a puddle toward a reflection it does not have. A surface marked as
// neither takes nothing -- SSR never traced it, and what the denoise bled into it from the ground
// below is not its reflection. Frames with nothing wet never reach this program, so the catcher's
// fold there is the plain tent's to the bit.
uniform sampler2D srcTex;     // the SSR buffer, premultiplied pairs
uniform sampler2D normalsTex; // the SSR marker in .a
uniform vec2 texelSize;       // one SSR-buffer texel

#include "ssr_marker.glsl"

bool catcherAt(vec2 uv) {
    return ssrMarkerIsCatcher(texture(normalsTex, uv).a);
}

void main()
{
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    if (!catcherAt(TexCoords)) {
        FragColor = vec4(0.0);
        return;
    }
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            if (!catcherAt(uv))
                continue;
            float k = KERNEL[x + 1] * KERNEL[y + 1];
            sum += texture(srcTex, uv) * k;
            total += k;
        }
    }
    // The centre tap is always a catcher, so the total is at least its weight.
    FragColor = sum / total;
}
