#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Debug overlay: draw a 2D texture straight to screen, in a frame corner. The
// shared one -- the atmosphere LUTs (--sky-debug) and the GI probe atlas
// (--gi-debug) both go through it, so keep it free of anything either-specific.
// scale makes small-magnitude HDR sources (multiscatter, sky-view) visible
// against the 8-bit framebuffer.
uniform sampler2D lut;
uniform float scale;

void main()
{
    FragColor = vec4(texture(lut, TexCoords).rgb * scale, 1.0);
}
