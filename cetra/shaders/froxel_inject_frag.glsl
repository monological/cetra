#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scattered radiance, a = extinction sigma

// Froxel fog, pass 1 of 3 (spec 9.5): evaluate the participating medium once
// per volume cell. The screen-space march this replaces evaluated the same
// lighting once per pixel per step, which made the clustered light list far too
// expensive to consult; a froxel pays for it once per cell.
//
// One draw per slice writes one layer of the volume (sliceIndex says which),
// so this is an ordinary fullscreen pass over a 160x90 grid, run 64 times.

#include "froxel.glsl"

uniform int sliceIndex;   // Which volume layer this draw is writing
uniform mat4 projection;  // Focal terms reconstruct the froxel's view position
uniform float fogFar;     // Far end of the volume's exponential depth range
uniform float density;    // Extinction at floor height (1/world units)

void main() {
    // Near plane recovered from the projection (the app's near clip varies) --
    // the same recovery contact_shadow_frag makes.
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    vec3 viewPos = froxelViewPos(TexCoords, float(sliceIndex), 0.5, nearZ, fogFar, invFocal);

    // M0: uniform medium, no lighting yet -- this exists to prove the volume
    // allocates, renders slice by slice, and composites on this driver.
    FragColor = vec4(vec3(0.0), density);
}
