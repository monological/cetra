#version 330 core

// A GRID fire's vorticity (spec 13.14): the curl of the velocity by central differences, and
// its magnitude in w, which the confinement takes the gradient of.

#include "fire_grid.glsl"
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
    vec3 xp = fireNeighbourVelocity(velocityTex, c + ivec3(1, 0, 0));
    vec3 xn = fireNeighbourVelocity(velocityTex, c - ivec3(1, 0, 0));
    vec3 yp = fireNeighbourVelocity(velocityTex, c + ivec3(0, 1, 0));
    vec3 yn = fireNeighbourVelocity(velocityTex, c - ivec3(0, 1, 0));
    vec3 zp = fireNeighbourVelocity(velocityTex, c + ivec3(0, 0, 1));
    vec3 zn = fireNeighbourVelocity(velocityTex, c - ivec3(0, 0, 1));
    float h2 = 2.0 * cell;
    vec3 w = vec3((yp.z - yn.z) - (zp.y - zn.y), (zp.x - zn.x) - (xp.z - xn.z),
                  (xp.y - xn.y) - (yp.x - yn.x)) /
             h2;
    outCurl = vec4(w, length(w));
}
