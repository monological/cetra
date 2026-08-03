#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Debug view of a tiling cloud-noise volume: one Z slice drawn into a
// screen-corner tile beside the sky LUT views. `channel` selects which
// field to inspect (0 = the Perlin-Worley/first octave the marcher
// thresholds, 1..3 the erosion octaves), shown as grayscale.
uniform sampler3D noiseTex;
uniform float slice; // 0..1 Z through the volume
uniform int channel; // 0..3

void main()
{
    vec4 n = texture(noiseTex, vec3(TexCoords, slice));
    FragColor = vec4(vec3(n[clamp(channel, 0, 3)]), 1.0);
}
