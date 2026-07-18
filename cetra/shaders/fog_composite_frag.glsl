#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Upsamples the half-res fog buffer (inscatter.rgb, transmittance.a) onto
// the HDR scene. Drawn with glBlendFunc(GL_ONE, GL_SRC_ALPHA):
// out = inscatter + scene * transmittance.
uniform sampler2D fogTex;
uniform vec2 texelSize; // One half-res fog texel

void main()
{
    // 3x3 tent: softens the march's per-pixel dither into a smooth local
    // average (fog is low-frequency; no depth-aware weights needed)
    const float KERNEL[3] = float[3](0.25, 0.5, 0.25);
    vec4 sum = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = TexCoords + vec2(float(x), float(y)) * texelSize;
            sum += texture(fogTex, uv) * (KERNEL[x + 1] * KERNEL[y + 1]);
        }
    }
    FragColor = sum;
}
