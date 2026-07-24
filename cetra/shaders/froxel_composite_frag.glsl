#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scatter to add, a = transmittance to multiply by

// Froxel fog, pass 3 of 3 (spec 9.5): fold the integrated volume into the HDR
// scene. Full res, one trilinear tap -- the reason the volume is a real 3D
// texture and not the codebase's usual 2D array, which cannot filter across
// slices.
//
// Output is NOT premultiplied: the caller blends with
// glBlendFunc(GL_ONE, GL_SRC_ALPHA), giving scene*T + inscatter, exactly the
// contract the screen-space fog composite used.

uniform sampler2D linDepthTex;      // Aux G-buffer; .z = linear view Z (<0), 0 = sky
uniform sampler3D integratedVolume;
uniform mat4 projection;
uniform float fogFar;

#include "depth.glsl" // viewPosFromLinZ; requires the projection uniform above
#include "froxel.glsl"

void main() {
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);
    float linZ = texture(linDepthTex, TexCoords).z;

    // Sky/background (aux sentinel 0) has no surface, so it takes the full
    // depth of the volume -- the far slice already holds the whole column.
    bool sky = (linZ >= -1e-4);
    // Euclidean view distance, matching what the screen-space march used as its
    // ray length, so the two agree about where a surface sits in the fog.
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    float viewDist = sky ? fogFar : length(viewPosFromLinZ(TexCoords, linZ, invFocal));

    float slice = froxelViewZToSlice(min(viewDist, fogFar), nearZ, fogFar);
    // Sample at the slice's centre in texture space; CLAMP_TO_EDGE on R holds
    // the final integrated value for anything at or past the last slice.
    vec3 uvw = vec3(TexCoords, (slice + 0.5) / float(FROXEL_Z));
    FragColor = texture(integratedVolume, uvw);
}
