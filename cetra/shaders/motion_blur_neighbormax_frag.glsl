#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Motion blur (4.15) neighbor-max: for each tile take the max-magnitude
// velocity over its 3x3 tile neighborhood, so a fast tile's blur reaches into
// adjacent tiles -- the reconstruction then blurs a fast object past its
// silhouette instead of cutting off hard at the object edge.
uniform sampler2D tileTex; // tile-max velocity (RG16F), one texel per tile
uniform vec2 tileTexel;    // 1 / tile-resolution

void main()
{
    vec2 maxVel = vec2(0.0);
    float maxLen2 = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 v = texture(tileTex, TexCoords + vec2(float(x), float(y)) * tileTexel).xy;
            float l2 = dot(v, v);
            if (l2 > maxLen2) {
                maxLen2 = l2;
                maxVel = v;
            }
        }
    }
    FragColor = vec4(maxVel, 0.0, 0.0);
}
