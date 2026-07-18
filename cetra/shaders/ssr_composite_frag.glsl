#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Lerps the half-res reflection buffer onto the HDR scene. The buffer is
// premultiplied (color * weight, weight) with strength and the [0,1] weight
// clamp already applied in the march; drawn with premultiplied-alpha
// blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) so a dark reflection darkens
// the floor beneath it — additive blending cannot.
uniform sampler2D ssrTex;
uniform vec2 texelSize; // One half-res reflection texel

void main()
{
    // 3x3 tent resolve: where the jittered march's hit/miss outcome is
    // marginal (grazing rays skimming thin geometry), the per-pixel jitter
    // decides it and the result inherits the noise pattern's diagonal
    // structure — visible once misses carry probe color instead of zero.
    // Averaging the premultiplied neighborhood turns that per-pixel
    // coin-flip into a smooth local hit fraction.
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    vec4 sum = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            sum += texture(ssrTex, uv) * (KERNEL[x + 1] * KERNEL[y + 1]);
        }
    }
    FragColor = sum;
}
