#version 330 core

// A GRID fire's projection (spec 13.14): the pressure's gradient taken out of the velocity,
// which leaves it divergence-free, and no velocity left pointing into a solid.

#include "fire_grid.glsl"
#include "fire_sim.glsl"

uniform sampler2D velocityTex;
uniform sampler2D pressureTex;
uniform float cell;

out vec4 outVelocity;

float pressureAt(ivec3 n, float here) {
    if (fireSolid(n))
        return here;
    return fireFetch(pressureTex, n, vec4(0.0)).x;
}

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outVelocity = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    float here = texelFetch(pressureTex, t, 0).x;
    vec3 grad = vec3(pressureAt(c + ivec3(1, 0, 0), here) - pressureAt(c - ivec3(1, 0, 0), here),
                     pressureAt(c + ivec3(0, 1, 0), here) - pressureAt(c - ivec3(0, 1, 0), here),
                     pressureAt(c + ivec3(0, 0, 1), here) - pressureAt(c - ivec3(0, 0, 1), here)) /
                (2.0 * cell);
    vec3 u = texelFetch(velocityTex, t, 0).xyz - grad;
    // Free slip: a wall stops what flows into it and lets what flows along it go.
    if (fireSolid(c + ivec3(1, 0, 0)))
        u.x = min(u.x, 0.0);
    if (fireSolid(c - ivec3(1, 0, 0)))
        u.x = max(u.x, 0.0);
    if (fireSolid(c + ivec3(0, 1, 0)))
        u.y = min(u.y, 0.0);
    if (fireSolid(c - ivec3(0, 1, 0)))
        u.y = max(u.y, 0.0);
    if (fireSolid(c + ivec3(0, 0, 1)))
        u.z = min(u.z, 0.0);
    if (fireSolid(c - ivec3(0, 0, 1)))
        u.z = max(u.z, 0.0);
    outVelocity = vec4(u, 0.0);
}
