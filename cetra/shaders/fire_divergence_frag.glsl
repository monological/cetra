#version 330 core

// A GRID fire's velocity divergence (spec 13.14), the right-hand side the pressure solve
// cancels: what the projection leaves is the divergence asked for, which is zero everywhere but
// the blue core. There the gas expands as it burns, Nguyen et al.'s density jump across the
// front (sec. 3.2), and asking for it is what pushes the flame out full rather than letting it
// rise as a thin column. A solid neighbour contributes a wall's zero velocity.

#include "fire_sim.glsl"

uniform sampler2D velocityTex;
uniform sampler2D scalarTex;
uniform float cell;
uniform float expansion; // 1/s

out vec4 outDivergence;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outDivergence = vec4(0.0);
        return;
    }
    float div = fireVelocityDifference(velocityTex, c, ivec3(1, 0, 0)).x +
                fireVelocityDifference(velocityTex, c, ivec3(0, 1, 0)).y +
                fireVelocityDifference(velocityTex, c, ivec3(0, 0, 1)).z;
    float grow = expansion * fireGas(texelFetch(scalarTex, fireAtlasTexel(c), 0)).core;
    outDivergence = vec4(div / (2.0 * cell) - grow, 0.0, 0.0, 0.0);
}
