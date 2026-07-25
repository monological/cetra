#version 330 core
out vec4 FragColor;

// DDGI gutter rewrite (spec 9.7): fill a tile's 1px border so a bilinear tap at
// the tile edge reads the octahedral map's true neighbour instead of bleeding in
// from whatever tile is packed beside it.
//
// The octahedral square wraps in a way no sampler understands: crossing an edge
// re-enters the SAME tile, mirrored along that edge and offset to the opposite
// side. Corners wrap to the diagonally opposite corner. This pass runs over the
// border ring only and copies each texel from its wrapped interior partner.

uniform sampler2D atlas;
uniform vec2 tileOrigin; // atlas texel of the tile's BORDER, lower-left
uniform float tileRes;   // interior edge length (border adds 1 on each side)

void main() {
    // Position within the bordered tile: 0 and tileRes+1 are the border ring.
    vec2 local = floor(gl_FragCoord.xy - tileOrigin);
    float outer = tileRes + 1.0;

    // Map the border texel to the interior texel it must mirror.
    vec2 src;
    bool cx = (local.x == 0.0 || local.x == outer);
    bool cy = (local.y == 0.0 || local.y == outer);

    // The pass covers the whole bordered tile because the ring is not a
    // rectangle; the interior is the projection's output and must survive.
    if (!cx && !cy)
        discard;

    if (cx && cy) {
        // Corner: the octahedron's diagonally opposite interior corner.
        src = vec2(local.x == 0.0 ? tileRes : 1.0, local.y == 0.0 ? tileRes : 1.0);
    } else if (cx) {
        // Vertical edge: mirror in y, step one column in from the far side.
        src = vec2(local.x == 0.0 ? 1.0 : tileRes, outer - local.y);
    } else {
        // Horizontal edge: mirror in x, step one row in from the far side.
        src = vec2(outer - local.x, local.y == 0.0 ? 1.0 : tileRes);
    }

    FragColor = texelFetch(atlas, ivec2(tileOrigin + src), 0);
}
