#version 330 core

// A GRID fire's sources, combustion and forces (spec 13.14), one step's worth, written as the
// two targets the advection reads next.
//
// Scalars are (temperature above ambient K, fuel, soot ppm, reaction in fuel per second).
// Fuel breathes in from the sources; where the air is past ignition it burns, a fraction of
// what is there each second, heating the air and leaving soot; the reaction is the rate it
// burnt at, which is what the blue base glows with. The air sheds heat as the fourth power of
// its rise (Nguyen et al. 2002), and soot oxidises fast in the flame's hot zone and fades slowly
// as smoke. The heat lifts the air (Boussinesq), and vorticity confinement puts back the curls
// the grid's own dissipation takes out (Fedkiw, Stam and Jensen 2001).

#include "fire_constants.glsl"
#include "fire_grid.glsl"
#include "fire_sim.glsl"
#include "pcg4d.glsl"

uniform sampler2D velocityTex;
uniform sampler2D scalarTex;
uniform sampler2D curlTex;
uniform float dt;
uniform float cell;
uniform vec3 boxMin; // world metres
uniform int stepIndex;

uniform float ambient;
uniform float ignition;
uniform float burnRate;
uniform float heat;
uniform float sootYield;
uniform float buoyancy;
uniform float sootWeight;
uniform float cooling;
uniform float vorticity;
uniform float sootBurnout;
uniform float sootBurnoutAt;
uniform float smokeFade;

// The sources: a = (centre or first end, shape), b = (half-extents or second end, radius),
// rate = (fuel per second, temperature K, lift m/s, unused).
uniform int sourceCount;
uniform vec4 sourceA[FIRE_MAX_SOURCES];
uniform vec4 sourceB[FIRE_MAX_SOURCES];
uniform vec4 sourceRate[FIRE_MAX_SOURCES];

layout(location = 0) out vec4 outVelocity;
layout(location = 1) out vec4 outScalars;

// Signed distance to a source's shape, metres: negative inside.
float sourceDistance(int i, vec3 w) {
    vec3 a = sourceA[i].xyz;
    vec3 b = sourceB[i].xyz;
    int shape = int(sourceA[i].w);
    if (shape == 0) {
        vec3 d = abs(w - a) - b;
        return max(d.x, max(d.y, d.z));
    }
    if (shape == 1)
        return length(w - a) - sourceB[i].w;
    vec3 ab = b - a;
    float s = clamp(dot(w - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
    return length(w - (a + s * ab)) - sourceB[i].w;
}

void main() {
    ivec3 c;
    if (!fireFragmentCell(c) || fireSolid(c)) {
        outVelocity = vec4(0.0);
        outScalars = vec4(0.0);
        return;
    }
    ivec2 t = fireAtlasTexel(c);
    vec3 u = texelFetch(velocityTex, t, 0).xyz;
    vec4 s = texelFetch(scalarTex, t, 0);
    float theta = max(s.x, 0.0);
    float fuel = max(s.y, 0.0);
    float soot = max(s.z, 0.0);
    vec3 w = boxMin + (vec3(c) + 0.5) * cell;

    // Each cell's draw of fuel varies from step to step, which is what the turbulence turns
    // into flicker; its mean is the authored rate.
    float jitter = 2.0 * float(pcg4d(uvec4(uvec3(c), uint(stepIndex))).x) / 4294967296.0;
    for (int i = 0; i < sourceCount; i++) {
        float cover = clamp(0.5 - sourceDistance(i, w) / cell, 0.0, 1.0);
        if (cover <= 0.0)
            continue;
        fuel += sourceRate[i].x * cover * jitter * dt;
        theta = max(theta, (sourceRate[i].y - ambient) * cover);
        u += (vec3(0.0, sourceRate[i].z, 0.0) - u) * cover * min(dt * 20.0, 1.0);
    }

    // Combustion, smooth across ignition so the flame front does not stair-step.
    float kelvin = ambient + theta;
    float lit = smoothstep(ignition - 50.0, ignition + 50.0, kelvin);
    float burnt = fuel * (1.0 - exp(-burnRate * dt)) * lit;
    fuel -= burnt;
    theta += heat * burnt;
    soot += sootYield * burnt;
    float reaction = burnt / dt;

    // Radiative cooling, the exact solution of d(theta)/dt = -a theta^4 over the step, so a
    // large step cannot overshoot below ambient.
    float a = cooling / (FIRE_COOLING_REF * FIRE_COOLING_REF * FIRE_COOLING_REF * FIRE_COOLING_REF);
    theta = theta / pow(1.0 + 3.0 * a * theta * theta * theta * dt, 1.0 / 3.0);

    float hot = smoothstep(sootBurnoutAt - 100.0, sootBurnoutAt + 100.0, ambient + theta);
    soot *= exp(-mix(smokeFade, sootBurnout, hot) * dt);

    // Buoyancy, g (T - T_amb) / T_amb, less the soot's weight.
    float lift = buoyancy * 9.81 * theta / ambient - sootWeight * soot;
    // Vorticity confinement: push along N x omega, N the direction |omega| grows in.
    vec4 omega = texelFetch(curlTex, t, 0);
    vec3 grad = vec3(fireFetch(curlTex, c + ivec3(1, 0, 0), vec4(0.0)).w -
                         fireFetch(curlTex, c - ivec3(1, 0, 0), vec4(0.0)).w,
                     fireFetch(curlTex, c + ivec3(0, 1, 0), vec4(0.0)).w -
                         fireFetch(curlTex, c - ivec3(0, 1, 0), vec4(0.0)).w,
                     fireFetch(curlTex, c + ivec3(0, 0, 1), vec4(0.0)).w -
                         fireFetch(curlTex, c - ivec3(0, 0, 1), vec4(0.0)).w);
    vec3 confine = vec3(0.0);
    float gl = length(grad);
    if (gl > 1e-6)
        confine = vorticity * cell * cross(grad / gl, omega.xyz);
    u += dt * (vec3(0.0, lift, 0.0) + confine);

    outVelocity = vec4(u, 0.0);
    outScalars = vec4(theta, fuel, soot, reaction);
}
