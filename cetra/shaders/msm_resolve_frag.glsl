#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // The four power moments of the depth in this texel's footprint

// Builds the moment cascades the surface shading samples under --msm, from the
// depth cascades the shadow pass already rendered. Spec 11.22.
//
// Deriving them here rather than writing moments in the depth pass is what keeps
// that pass -- its polygon offset, its front-face culling, its colour-less FBO --
// byte-identical between --msm and the default, so the off path is provably the
// path that was there before. The cost is one extra read of a map that is already
// resident. The only thing writing them in the depth pass would buy is MSAA inside
// the map, which a larger map buys as well.
//
// Structure mirrors fog_esm_frag: one shader, two modes, three draws per layer.

#include "msm.glsl"

uniform sampler2DArray srcDepth;   // mode 0: the scene's depth cascades
uniform sampler2DArray srcMoments; // mode 1: this buffer, mid-blur
uniform int layer;                 // Array layer being read
uniform vec2 blurStep;             // mode 1: per-tap spacing, 0 on the other axis
uniform int mode;                  // 0 = downsample + form moments, 1 = separable blur

void main() {
    if (mode == 0) {
        // A 2x2 box in the SOURCE, which is what makes this a downsample rather
        // than a point resample: the target is a fraction of the source size, so a
        // single tap would alias in exactly the frequencies a filterable
        // representation exists to remove.
        //
        // Moments are formed per tap and then averaged. The reverse -- averaging
        // the four depths and taking the moments of the mean -- would throw away
        // the spread inside the footprint, which is the entire quantity the
        // reconstruction reads to place a penumbra.
        vec2 texel = 1.0 / vec2(textureSize(srcDepth, 0).xy);
        vec4 sum = vec4(0.0);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                vec2 uv = TexCoords + (vec2(x, y) - 0.5) * texel;
                sum += msmMoments(
                    msmWarpDepth(texture(srcDepth, vec3(uv, float(layer))).r));
            }
        }
        FragColor = sum * 0.25;
        return;
    }

    // Separable box, one axis per draw. Linear in the stored quantity, which is
    // legal precisely because the quantity is moments and not depths.
    //
    // Five taps at a spacing the caller sets. Widening is the knob, but only up to
    // about one texel per tap: past that the fixed tap count stops covering the
    // span it straddles and the filter combs rather than blurs.
    vec4 sum = vec4(0.0);
    for (int i = -2; i <= 2; i++) {
        vec2 uv = TexCoords + blurStep * float(i);
        sum += texture(srcMoments, vec3(uv, float(layer)));
    }
    FragColor = sum * 0.2;
}
