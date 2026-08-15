#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Debug overlay: draw a 2D texture straight to screen, in a frame corner. The
// shared one -- the atmosphere LUTs (--sky-debug) and the GI probe atlas
// (--gi-debug) both go through it, so keep it free of anything either-specific.
// scale makes small-magnitude HDR sources (multiscatter, sky-view) visible
// against the 8-bit framebuffer.
// mono describes the TEXTURE, not either caller: a single-channel source has nothing in
// .gb, so sampling .rgb would draw it as a red tile rather than as the scalar field it is.
// Same reading cloud_noise_debug_frag gives its 3D fields.
uniform sampler2D lut;
uniform float scale;
uniform int mono;

void main()
{
    vec3 c = texture(lut, TexCoords).rgb;
    FragColor = vec4((mono != 0 ? c.rrr : c) * scale, 1.0);
}
