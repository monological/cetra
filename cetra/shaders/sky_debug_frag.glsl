#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Debug overlay: draw a LUT texture straight to screen (used by
// --sky-debug to inspect the atmosphere LUTs in a frame corner). scale
// makes small-magnitude HDR LUTs (multiscatter, sky-view) visible against
// the 8-bit framebuffer.
uniform sampler2D lut;
uniform float scale;

void main()
{
    FragColor = vec4(texture(lut, TexCoords).rgb * scale, 1.0);
}
