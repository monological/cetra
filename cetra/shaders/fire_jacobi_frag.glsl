#version 330 core

// One Jacobi iteration of a GRID fire's pressure solve (spec 13.14): the Poisson equation
// lap(p) = div(u), with the compact 7-point Laplacian, as GPU Gems 3 ch. 30 solves it.
//
// The projection then takes the CENTRAL-difference gradient, whose divergence is the wide
// Laplacian, not this one, so some divergence survives the solve. The wide stencil was tried
// and is the worse error: on a collocated grid it decouples alternating cells, and the
// pressure's checkerboard reached the velocity as a speckle that a bake's fine grid printed into
// every frame. A solid neighbour mirrors this cell's pressure, the wall's zero-flux condition; an
// open face is held at zero, so the box's air can leave through it.

#include "fire_grid.glsl"
#include "fire_sim.glsl"

uniform sampler2D pressureTex;
uniform sampler2D divergenceTex;
uniform float cell;

out vec4 outPressure;

float neighbour(ivec3 n, float here) {
    if (fireSolid(n))
        return here;
    return fireFetch(pressureTex, n, vec4(0.0)).x;
}

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outPressure = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    float here = texelFetch(pressureTex, t, 0).x;
    float sum = neighbour(c + ivec3(1, 0, 0), here) + neighbour(c - ivec3(1, 0, 0), here) +
                neighbour(c + ivec3(0, 1, 0), here) + neighbour(c - ivec3(0, 1, 0), here) +
                neighbour(c + ivec3(0, 0, 1), here) + neighbour(c - ivec3(0, 0, 1), here);
    outPressure = vec4((sum - cell * cell * texelFetch(divergenceTex, t, 0).x) / 6.0, 0.0, 0.0, 0.0);
}
