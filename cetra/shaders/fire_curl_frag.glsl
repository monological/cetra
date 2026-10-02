#version 330 core

// A GRID fire's vorticity (spec 13.14): the curl of the velocity by central differences, and
// its magnitude in w, which the confinement takes the gradient of.

#include "fire_sim.glsl"

uniform sampler2D velocityTex;
uniform float cell;

out vec4 outCurl;

void main() {
    ivec3 c;
    if (!fireFragmentCell(c)) {
        outCurl = vec4(0.0);
        return;
    }
    vec3 dx = fireVelocityDifference(velocityTex, c, ivec3(1, 0, 0));
    vec3 dy = fireVelocityDifference(velocityTex, c, ivec3(0, 1, 0));
    vec3 dz = fireVelocityDifference(velocityTex, c, ivec3(0, 0, 1));
    vec3 w = vec3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x) / (2.0 * cell);
    outCurl = vec4(w, length(w));
}
