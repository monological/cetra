// A fire's grid as a flat 2D atlas of its z slices (spec 13.14; Harris, "Fast Fluid Dynamics
// Simulation on the GPU", GPU Gems 1 ch. 38). Slice z is the tile at (z % tilesX, z / tilesX),
// gridSize.x by gridSize.y texels, so a whole grid is one render target and every pass over it
// is one draw -- where 3D textures written a slice at a time cost a draw, and on this driver a
// render pass, per slice per pass.
//
// Cell coordinates are integers; a continuous position `p` is in cells with cell (i, j, k)'s
// centre at p = (i, j, k) + 0.5.

#include "fire_constants.glsl"

// The grid's cells in xyz and the atlas's tiles across in w, one uniform.
uniform ivec4 gridDims;
#define gridSize (gridDims.xyz)
#define tilesX (gridDims.w)
uniform int floorSolid; // 1 = the box's bottom face is a floor, not an open face

// The gas a cell holds, as the scalar field stores it in its four channels.
struct FireGas {
    float rise; // K above ambient
    float Y;    // Nguyen's reaction coordinate: 1 at the front, falling as the gas burns
    float soot; // ppm
    float core; // 0..1, the blue core's weight
};

FireGas fireGas(vec4 s) {
    return FireGas(s.x, s.y, s.z, s.w);
}

vec4 fireGasPack(FireGas g) {
    return vec4(g.rise, g.Y, g.soot, g.core);
}

// The gas at continuous position `p` as it is drawn and cast: its soot and core thinned to
// nothing over the last FIRE_EDGE_FADE_CELLS before an open face of the box. fire.c's
// fire_edge_fade is the same.
FireGas fireFadeAtEdge(FireGas g, vec3 p) {
    vec3 far = vec3(gridSize) - p;
    float edge = min(min(min(p.x, far.x), min(p.z, far.z)), far.y);
    if (floorSolid == 0)
        edge = min(edge, p.y);
    float fade = smoothstep(0.0, FIRE_EDGE_FADE_CELLS, edge);
    g.soot *= fade;
    g.core *= fade;
    return g;
}

// The atlas texel slice z's tile starts at, and the slice a tile holds.
ivec2 fireTileOrigin(int z) {
    return ivec2((z % tilesX) * gridSize.x, (z / tilesX) * gridSize.y);
}

int fireTileSlice(ivec2 tile) {
    return tile.y * tilesX + tile.x;
}

ivec2 fireAtlasTexel(ivec3 c) {
    return fireTileOrigin(c.z) + c.xy;
}

// The cell this fragment writes. False on the atlas's padding past the last slice.
bool fireFragmentCell(out ivec3 c) {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 tile = p / gridSize.xy;
    c = ivec3(p - tile * gridSize.xy, fireTileSlice(tile));
    return c.z < gridSize.z;
}

bool fireInGrid(ivec3 c) {
    return all(greaterThanEqual(c, ivec3(0))) && all(lessThan(c, gridSize));
}

// One cell's texel, or `outside` past the grid.
vec4 fireFetch(sampler2D t, ivec3 c, vec4 outside) {
    return fireInGrid(c) ? texelFetch(t, fireAtlasTexel(c), 0) : outside;
}

// Trilinear at continuous position `p`: a hardware-bilinear tap in each of the two slices
// either side, mixed. The tap stays half a texel inside its tile, so it never blends in the
// neighbouring slice that sits beside it in the atlas -- which holds the grid's edge cells'
// values out to the edge, as CLAMP_TO_EDGE would. Past the grid is `outside`, but under a solid
// floor is the floor: what is there is the bottom row's, not the air's.
vec4 fireSample(sampler2D t, vec3 p, vec4 outside) {
    if (floorSolid != 0)
        p.y = max(p.y, 0.0);
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(gridSize))))
        return outside;
    vec2 xy = clamp(p.xy, vec2(0.5), vec2(gridSize.xy) - 0.5);
    float z = clamp(p.z - 0.5, 0.0, float(gridSize.z - 1));
    int z0 = int(floor(z));
    int z1 = min(z0 + 1, gridSize.z - 1);
    vec2 atlas = vec2(textureSize(t, 0));
    vec2 uv0 = (vec2(fireTileOrigin(z0)) + xy) / atlas;
    vec2 uv1 = (vec2(fireTileOrigin(z1)) + xy) / atlas;
    return mix(textureLod(t, uv0, 0.0), textureLod(t, uv1, 0.0), z - float(z0));
}
