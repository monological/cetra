#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Shared 3x3 tent upsample of a half-res effect buffer onto the full-res
// HDR scene. The call site's blend state defines the composite: SSR draws
// its premultiplied (color * weight, weight) buffer with (GL_ONE,
// GL_ONE_MINUS_SRC_ALPHA) so a dark reflection can darken the floor
// beneath it; fog draws (inscatter, transmittance) with (GL_ONE,
// GL_SRC_ALPHA) so the output is inscatter + scene * transmittance. The
// tent matters where a jittered march's per-pixel outcome is marginal
// (hit-or-miss grazing reflections, dithered step positions): averaging
// the neighborhood turns that per-pixel coin-flip into a smooth local
// fraction instead of structured noise.
uniform sampler2D srcTex;
uniform vec2 texelSize; // One half-res source texel

void main()
{
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    vec4 sum = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            sum += texture(srcTex, uv) * (KERNEL[x + 1] * KERNEL[y + 1]);
        }
    }
    FragColor = sum;
}
