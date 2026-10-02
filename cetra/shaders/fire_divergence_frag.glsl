#version 330 core

// A GRID fire's velocity divergence (spec 13.14), the right-hand side the pressure solve
// cancels. A solid neighbour contributes a wall's zero velocity; an open face the wind.

#include "fire_grid.glsl"
#include "fire_sim.glsl"

uniform sampler2D velocityTex;
uniform float cell;

out vec4 outDivergence;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outDivergence = vec4(0.0);
        return;
    }
    float div = fireNeighbourVelocity(velocityTex, c + ivec3(1, 0, 0)).x -
                fireNeighbourVelocity(velocityTex, c - ivec3(1, 0, 0)).x +
                fireNeighbourVelocity(velocityTex, c + ivec3(0, 1, 0)).y -
                fireNeighbourVelocity(velocityTex, c - ivec3(0, 1, 0)).y +
                fireNeighbourVelocity(velocityTex, c + ivec3(0, 0, 1)).z -
                fireNeighbourVelocity(velocityTex, c - ivec3(0, 0, 1)).z;
    outDivergence = vec4(div / (2.0 * cell), 0.0, 0.0, 0.0);
}
