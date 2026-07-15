#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Lerps the half-res reflection buffer onto the HDR scene. The buffer is
// premultiplied (color * weight, weight) with strength and the [0,1] weight
// clamp already applied in the march; drawn with premultiplied-alpha
// blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) so a dark reflection darkens
// the floor beneath it — additive blending cannot.
uniform sampler2D ssrTex;

void main()
{
    FragColor = texture(ssrTex, TexCoords);
}
