#version 330 core
in vec2 TexCoords;

// Alpha-masked materials (hair cards, foliage) must not cast shadows from
// their transparent regions: without this test every card shadows as its
// full solid quad, drawing card-shaped dark streaks across the geometry
// beneath (invisible at normal distance, obvious at close range)
uniform sampler2D albedoTex;
uniform int alphaTest;
uniform float alphaCutoff;

void main()
{
    if (alphaTest == 1 && texture(albedoTex, TexCoords).a < alphaCutoff) {
        discard;
    }
    // Depth is written automatically to gl_FragDepth
}
