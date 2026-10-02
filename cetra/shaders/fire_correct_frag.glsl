#version 330 core

// MacCormack's correction for a GRID fire's advection (spec 13.14; Selle et al. 2008): the
// forward trace phi^ plus half of what tracing it back lost, phi^ + (phi - phi_back) / 2, clamped
// to the cells the forward trace landed among -- the clamp is what makes the scheme
// unconditionally stable.

#include "fire_sim.glsl"

uniform sampler2D velocityTex;  // u^n, as the forward trace followed it
uniform sampler2D fromVelocity; // phi^, the forward trace
uniform sampler2D fromScalars;
uniform sampler2D baseVelocity; // phi^n itself
uniform sampler2D baseScalars;
uniform sampler2D backVelocity; // phi_back, traced back from phi^
uniform sampler2D backScalars;
uniform float dt;
uniform float cell;

layout(location = 0) out vec4 outVelocity;
layout(location = 1) out vec4 outScalars;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outVelocity = vec4(0.0);
        outScalars = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    vec3 u = texelFetch(velocityTex, t, 0).xyz;
    vec3 p = vec3(c) + 0.5 - u * dt / cell;
    vec4 air = vec4(wind, 0.0);
    vec4 v = texelFetch(fromVelocity, t, 0) +
             0.5 * (texelFetch(baseVelocity, t, 0) - texelFetch(backVelocity, t, 0));
    vec4 s = texelFetch(fromScalars, t, 0) +
             0.5 * (texelFetch(baseScalars, t, 0) - texelFetch(backScalars, t, 0));
    ivec3 lo = ivec3(floor(p - 0.5));
    vec4 vmin = vec4(1e30), vmax = vec4(-1e30), smin = vec4(1e30), smax = vec4(-1e30);
    for (int k = 0; k < 8; k++) {
        ivec3 n = lo + ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        vec4 nv = fireFetch(baseVelocity, n, air);
        vec4 ns = fireFetch(baseScalars, n, vec4(0.0));
        vmin = min(vmin, nv);
        vmax = max(vmax, nv);
        smin = min(smin, ns);
        smax = max(smax, ns);
    }
    outVelocity = clamp(v, vmin, vmax);
    outScalars = clamp(s, smin, smax);
}
