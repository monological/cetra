#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Lerps the half-res reflection buffer onto the HDR scene. The buffer is
// premultiplied (color * weight, weight); drawn with premultiplied-alpha
// blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) so a dark reflection darkens
// the floor beneath it — additive blending cannot.
uniform sampler2D ssrTex;
uniform float strength;

void main()
{
    vec4 ssr = texture(ssrTex, TexCoords);
    FragColor = vec4(ssr.rgb * strength, clamp(ssr.a * strength, 0.0, 1.0));
}
