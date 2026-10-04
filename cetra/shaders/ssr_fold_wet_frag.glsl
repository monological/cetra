#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// The SSR fold for a frame with wet ground (spec 13.9), in place of upsample_tent_frag's plain
// tent, under the same (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) blend.
//
// Wet ground is a real surface, and SSR must REPLACE its share of the environment's reflection
// rather than lerp the whole pixel toward the trace: a lerp dims the pixel's diffuse by a
// Fresnel the lit shader already applied, and keeps the environment's reflection under the one
// that replaced it. So a wet pixel's pair carries the reflection with its Fresnel on the colour
// and its coverage bare -- the fraction of the environment's reflection the trace or the probe
// stands in for -- and here the frame gains the one and loses that fraction of the other: of
// the ambient specular as the split composite put it back, under its occlusion, which is read
// through the composite's own statement of it. With alpha 0 the blend adds, so the diffuse is
// untouched.
//
// The catcher keeps the lerp, and the tent averages each class only with its own: a wet pair's
// bare coverage folded into the wall beside a puddle would darken the wall by it. A surface
// marked as neither takes nothing -- SSR never traced it, and what the denoise bled into it
// from the wet ground below is not its reflection. Frames with nothing wet never reach this
// program, so the catcher's fold there is the plain tent's to the bit.
uniform sampler2D srcTex;  // the SSR buffer, premultiplied pairs (see above)
uniform sampler2D specTex; // the ambient specular, working space
uniform vec2 texelSize;    // one SSR-buffer texel

#include "split_occlusion.glsl"

int classAt(vec2 uv) {
    return ssrMarkerClass(texture(normalsTex, uv).a);
}

void main()
{
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    int cls = classAt(TexCoords);
    if (cls == 0) {
        FragColor = vec4(0.0);
        return;
    }
    bool wet = cls == 2;
    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            if (classAt(uv) != cls)
                continue;
            float k = KERNEL[x + 1] * KERNEL[y + 1];
            sum += texture(srcTex, uv) * k;
            total += k;
        }
    }
    // The centre tap is always its own class, so the total is at least its weight.
    sum /= total;
    if (!wet) {
        FragColor = sum;
        return;
    }
    vec3 putBack = texture(specTex, TexCoords).rgb * splitOcclusionAt(TexCoords).x;
    FragColor = vec4(sum.rgb - sum.a * putBack, 0.0);
}
