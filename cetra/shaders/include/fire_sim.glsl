// What every simulation pass of a GRID fire shares (spec 13.14): the solids, the air past the
// box, and the six-neighbour stencils the passes take differences over.

#include "fire_grid.glsl"

uniform sampler2D obstacleTex; // R8, 1 inside a solid
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

// The velocity's central difference across the axis `e` points along, unscaled: u(c + e) less
// u(c - e). The curl and the divergence are both built from the three.
vec3 fireVelocityDifference(sampler2D velocity, ivec3 c, ivec3 e) {
    return fireNeighbourVelocity(velocity, c + e) - fireNeighbourVelocity(velocity, c - e);
}

// Neighbour n's pressure, `wall` its fireSolid: a solid mirrors `here`, the wall's zero-flux
// condition, and an open face is held at zero, so the box's air can leave through it. The solve
// and the gradient have to agree on this, so both read it here.
float firePressureNeighbour(sampler2D pressure, ivec3 n, bool wall, float here) {
    return wall ? here : fireFetch(pressure, n, vec4(0.0)).x;
}
