#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// DoF tile dilate: channelwise max over the (2K+1)^2 tile neighborhood, so a
// defocused tile's reach extends into every tile its blur can spill into.
// K is sized CPU-side from maxCoC (a blur of C half-res texels spills at most
// ceil(C / tileSize) tiles), unlike motion blur's fixed 3x3 -- DoF's reach is
// a user knob, not a physical shutter bound.
uniform sampler2D tileTex; // per-tile (maxFarCoC, maxNearCoC), RG16F
uniform vec2 tileTexel;    // 1 / tile-resolution
uniform int dilateRadius;  // K

void main()
{
    vec2 m = vec2(0.0);
    for (int y = -dilateRadius; y <= dilateRadius; y++) {
        for (int x = -dilateRadius; x <= dilateRadius; x++) {
            m = max(m, texture(tileTex, TexCoords + vec2(float(x), float(y)) * tileTexel).rg);
        }
    }
    FragColor = vec4(m, 0.0, 0.0);
}
