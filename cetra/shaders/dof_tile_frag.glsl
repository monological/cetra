#version 330 core
// post_vert emits TexCoords; this pass addresses by gl_FragCoord instead, so
// it is deliberately not declared here (an unread stage input is legal).
out vec4 FragColor;

// DoF tile-max: reduce the half-res signed CoC to per-tile maxima, far and
// near kept separate -- R = max background CoC, G = max foreground CoC over
// the tile. Feeds the dilate -> the gather's per-pixel kernel radius, so a
// defocused neighbour's blur reaches pixels its own CoC never touches (the
// near field's spill-past-the-silhouette depends on it). tileSize is a
// uniform so the 2D loop stays rolled; it must equal DOF_TILE in postfx.c.
uniform sampler2D cocColorTex; // Half-res scene colour (rgb) + signed CoC (a)
uniform vec2 texelSize;        // 1 / half-res
uniform int tileSize;          // DOF_TILE

void main()
{
    // gl_FragCoord.xy indexes this tile; its base half-res texel is tile*tileSize.
    vec2 base = floor(gl_FragCoord.xy) * float(tileSize);
    float maxFar = 0.0;
    float maxNear = 0.0;
    for (int y = 0; y < tileSize; y++) {
        for (int x = 0; x < tileSize; x++) {
            // cocColorTex is CLAMP_TO_EDGE, so a partial-tile sample past uv=1
            // already resolves to the edge texel -- no explicit guard needed.
            float coc =
                texture(cocColorTex, (base + vec2(float(x) + 0.5, float(y) + 0.5)) * texelSize).a;
            maxFar = max(maxFar, coc);
            maxNear = max(maxNear, -coc);
        }
    }
    FragColor = vec4(maxFar, maxNear, 0.0, 0.0);
}
