#version 330 core

// One Jacobi iteration of a GRID fire's pressure solve (spec 13.14).
//
// The Laplacian is the WIDE one, (p(c + 2e) - 2 p(c) + p(c - 2e)) / (2h)^2 along each axis,
// because it has to be the divergence of the gradient the projection takes: both are central
// differences, and the composition of two central differences reaches two cells out. Solving
// the compact 7-point Laplacian instead leaves the projected velocity's central divergence far
// from zero however long the solve runs -- the collocated grid's checkerboard, which the
// compact stencil cannot see. So u - grad(p) is divergence-free in exactly the sense the
// divergence pass measures.
//
// A solid on the way out mirrors this cell's pressure (the wall's zero-flux condition); past an
// open face the pressure is held at zero, so the box's air can leave through it.

#include "fire_grid.glsl"
#include "fire_sim.glsl"

uniform sampler2D pressureTex;
uniform sampler2D divergenceTex;
uniform float cell;

out vec4 outPressure;

// The pressure two cells out, as the projection will see it: a solid next door takes no
// correction, so its term drops (this cell's own pressure); a solid two out is mirrored from the
// cell between, as the projection's gradient mirrors it.
float neighbour(ivec3 c, ivec3 e, float here) {
    if (fireSolid(c + e))
        return here;
    if (fireSolid(c + 2 * e))
        return fireFetch(pressureTex, c + e, vec4(0.0)).x;
    return fireFetch(pressureTex, c + 2 * e, vec4(0.0)).x;
}

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outPressure = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    float here = texelFetch(pressureTex, t, 0).x;
    float sum = neighbour(c, ivec3(1, 0, 0), here) + neighbour(c, ivec3(-1, 0, 0), here) +
                neighbour(c, ivec3(0, 1, 0), here) + neighbour(c, ivec3(0, -1, 0), here) +
                neighbour(c, ivec3(0, 0, 1), here) + neighbour(c, ivec3(0, 0, -1), here);
    float h2 = 4.0 * cell * cell;
    outPressure = vec4((sum - h2 * texelFetch(divergenceTex, t, 0).x) / 6.0, 0.0, 0.0, 0.0);
}
