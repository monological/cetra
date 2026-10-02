// What every simulation pass of a GRID fire shares (spec 13.14): the solids, and the air past
// the box. Include after fire_grid.glsl.

uniform sampler2D obstacleTex; // R8, 1 inside a solid
uniform int floorSolid;        // 1 = the cells under the box are solid too
uniform vec3 wind;             // m/s, the air that blows in where the box is open

bool fireSolid(ivec3 c) {
    if (c.y < 0)
        return floorSolid != 0;
    if (!fireInGrid(c))
        return false;
    return texelFetch(obstacleTex, fireAtlasTexel(c), 0).r > 0.5;
}

// A neighbour's velocity for a finite difference: still at a solid; past an open face, the edge
// cell's own (zero gradient), so air leaves the box as freely as it moves inside it. The wind
// comes in through the advection, which carries it from past the face.
vec3 fireNeighbourVelocity(sampler2D velocity, ivec3 c) {
    if (fireSolid(c))
        return vec3(0.0);
    return texelFetch(velocity, fireAtlasTexel(clamp(c, ivec3(0), gridSize - 1)), 0).xyz;
}
