#version 330 core

// A GRID fire's advection (spec 13.14): velocity and scalars carried along the velocity
// together, written as two targets. Three modes make MacCormack (Selle et al. 2008):
//   0  forward: phi^ = phi(x - u dt), the semi-Lagrangian step (Stam 1999)
//   1  backward: phi_back = phi^(x + u dt), the same step reversed from the forward result
//   2  correct: phi^ + (phi - phi_back) / 2, clamped to the cells the backtrace landed among
// Semi-Lagrangian alone is mode 0 written straight to the result.

#include "fire_grid.glsl"
#include "fire_sim.glsl"

uniform sampler2D velocityTex; // u^n, the velocity the backtrace follows
uniform sampler2D fromVelocity; // what is carried: phi in mode 0, phi^ in modes 1 and 2
uniform sampler2D fromScalars;
uniform sampler2D baseVelocity; // mode 2: phi^n itself
uniform sampler2D baseScalars;
uniform sampler2D backVelocity; // mode 2: phi_back
uniform sampler2D backScalars;
uniform int mode;
uniform float dt;
uniform float cell; // metres

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
    vec3 here = vec3(c) + 0.5;
    vec4 air = vec4(wind, 0.0);
    if (mode == 1) {
        vec3 p = here + u * dt / cell;
        outVelocity = fireSample(fromVelocity, p, air);
        outScalars = fireSample(fromScalars, p, vec4(0.0));
        return;
    }
    vec3 p = here - u * dt / cell;
    if (mode == 0) {
        outVelocity = fireSample(fromVelocity, p, air);
        outScalars = fireSample(fromScalars, p, vec4(0.0));
        return;
    }
    vec4 v = texelFetch(fromVelocity, t, 0) +
             0.5 * (texelFetch(baseVelocity, t, 0) - texelFetch(backVelocity, t, 0));
    vec4 s = texelFetch(fromScalars, t, 0) +
             0.5 * (texelFetch(baseScalars, t, 0) - texelFetch(backScalars, t, 0));
    // The correction may not leave the range of the cells the backtrace interpolated between:
    // that clamp is what makes the scheme unconditionally stable.
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
