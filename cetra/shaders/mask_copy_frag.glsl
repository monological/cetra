#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Copy a source mask texture into one layer of the material mask array,
// resampling to the array's canonical size via the source sampler's linear
// filtering. All four channels pass through unchanged so per-mask channel
// reads downstream (roughness .g, metallic .b, ao/opacity/... .r) stay
// faithful; a source already at the canonical size copies 1:1.
uniform sampler2D src;

void main()
{
    FragColor = texture(src, TexCoords);
}
