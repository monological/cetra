#version 330 core

// One Jacobi iteration of a GRID fire's pressure solve (spec 13.14): the Poisson equation
// lap(p) = div(u), with the compact 7-point Laplacian, as GPU Gems 3 ch. 30 solves it.
//
// The projection then takes the CENTRAL-difference gradient, whose divergence is the wide
// Laplacian, not this one, so some divergence survives the solve. The wide stencil was tried
// and is the worse error: on a collocated grid it decouples alternating cells, and the
// pressure's checkerboard reached the velocity as a speckle that a fine grid printed into every
// frame.

#include "fire_sim.glsl"

uniform sampler2D pressureTex;
uniform sampler2D divergenceTex;
uniform float cell;

out vec4 outPressure;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outPressure = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    float here = texelFetch(pressureTex, t, 0).x;
    float sum = 0.0;
    for (int a = 0; a < 3; a++) {
        ivec3 e = ivec3(0);
        e[a] = 1;
        sum += firePressureNeighbour(pressureTex, c + e, fireSolid(c + e), here);
        sum += firePressureNeighbour(pressureTex, c - e, fireSolid(c - e), here);
    }
    outPressure = vec4((sum - cell * cell * texelFetch(divergenceTex, t, 0).x) / 6.0, 0.0, 0.0, 0.0);
}
