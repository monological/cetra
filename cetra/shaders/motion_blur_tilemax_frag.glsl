#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Motion blur (4.15) tile-max: reduce the full-res velocity buffer to one
// max-magnitude velocity per tileSize x tileSize tile. Feeds neighbor-max ->
// the reconstruction's blur extent, so a fast object bleeds past its
// silhouette. tileSize is a uniform so the 2D loop stays rolled (small shader);
// it must equal MOTION_BLUR_TILE in postfx.c.
uniform sampler2D auxTex; // full-res aux: .xy velocity (UV units)
uniform vec2 auxTexel;    // 1 / full-res
uniform int tileSize;     // MOTION_BLUR_TILE

void main()
{
    // gl_FragCoord.xy indexes this tile; its base full-res pixel is tile*tileSize.
    vec2 base = floor(gl_FragCoord.xy) * float(tileSize);
    vec2 maxVel = vec2(0.0);
    float maxLen2 = 0.0;
    for (int y = 0; y < tileSize; y++) {
        for (int x = 0; x < tileSize; x++) {
            // Clamp so the last (partial) tile never wraps to the opposite edge.
            vec2 uv = clamp((base + vec2(float(x) + 0.5, float(y) + 0.5)) * auxTexel,
                            vec2(0.0), vec2(1.0));
            vec2 v = texture(auxTex, uv).xy;
            float l2 = dot(v, v);
            if (l2 > maxLen2) {
                maxLen2 = l2;
                maxVel = v;
            }
        }
    }
    FragColor = vec4(maxVel, 0.0, 0.0);
}
