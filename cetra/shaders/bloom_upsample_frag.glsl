#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// One bloom-pyramid upsample step: 3x3 tent of the next-coarser level.
// Drawn with additive blending (GL_ONE, GL_ONE) so the widened coarse glow
// accumulates onto the destination level's own downsampled content -- the
// progressive accumulation is what turns the mip chain into one wide,
// smooth kernel instead of a stack of visible rings.
uniform sampler2D srcTex;
uniform vec2 texelSize; // One SOURCE-level (coarser) texel

void main()
{
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            sum += texture(srcTex, uv).rgb * (KERNEL[x + 1] * KERNEL[y + 1]);
        }
    }
    FragColor = vec4(sum, 1.0);
}
