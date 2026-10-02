#version 330 core

// A GRID fire's projection (spec 13.14): the pressure's gradient taken out of the velocity,
// which leaves it divergence-free, and no velocity left pointing into a solid.

#include "fire_sim.glsl"

uniform sampler2D velocityTex;
uniform sampler2D pressureTex;
uniform float cell;

out vec4 outVelocity;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outVelocity = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    float here = texelFetch(pressureTex, t, 0).x;
    vec3 grad;
    bvec3 wallAbove, wallBelow;
    for (int a = 0; a < 3; a++) {
        ivec3 e = ivec3(0);
        e[a] = 1;
        wallAbove[a] = fireSolid(c + e);
        wallBelow[a] = fireSolid(c - e);
        grad[a] = firePressureNeighbour(pressureTex, c + e, wallAbove[a], here) -
                  firePressureNeighbour(pressureTex, c - e, wallBelow[a], here);
    }
    vec3 u = texelFetch(velocityTex, t, 0).xyz - grad / (2.0 * cell);
    // Free slip: a wall stops what flows into it and lets what flows along it go.
    for (int a = 0; a < 3; a++) {
        if (wallAbove[a])
            u[a] = min(u[a], 0.0);
        if (wallBelow[a])
            u[a] = max(u[a], 0.0);
    }
    outVelocity = vec4(u, 0.0);
}
