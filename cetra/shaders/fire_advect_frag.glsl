#version 330 core

// A GRID fire's advection (spec 13.14): velocity and scalars carried along the velocity
// together, written as two targets -- the semi-Lagrangian trace phi(x - u dt) (Stam 1999).
// MacCormack (Selle et al. 2008) runs it twice, forward and then back from the forward result
// with dt negated, and fire_correct_frag combines the three.

#include "fire_sim.glsl"

uniform sampler2D velocityTex;  // u^n, the velocity the trace follows
uniform sampler2D fromVelocity; // what is carried
uniform sampler2D fromScalars;
uniform float dt;   // seconds, negative for the backward trace
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
    vec3 u = texelFetch(velocityTex, fireAtlasTexel(c), 0).xyz;
    vec3 p = vec3(c) + 0.5 - u * dt / cell;
    outVelocity = fireSample(fromVelocity, p, vec4(wind, 0.0));
    outScalars = fireSample(fromScalars, p, vec4(0.0));
}
