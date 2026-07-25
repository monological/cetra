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
uniform int froxelDepth; // Slice count; mirrors POSTFX_FROXEL_Z

#include "froxel.glsl"

void main() {
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);
    float linZ = texture(linDepthTex, TexCoords).z;

    // Sky/background (aux sentinel 0) has no surface, so it takes the full
    // depth of the volume -- the far slice already holds the whole column.
    bool sky = (linZ >= -1e-4);
    // PLANAR depth, not the ray length the screen-space march used: the volume's
    // slices are constant-depth planes (froxelViewPos puts every cell of a slice
    // at the same view z), so feeding it a radial distance would index deeper
    // the further a pixel sits off the optical axis.
    float depth = sky ? fogFar : -linZ;

    float slice = froxelViewZToSlice(min(depth, fogFar), nearZ, fogFar, float(froxelDepth));
    // Layer s holds the integral out to the FAR face of slice s (the integrate
    // pass ends its accumulation at slice s+1), so a depth at continuous slice
    // coordinate `slice` is carried by layer slice-1, whose texel centre sits at
    // (slice-0.5)/depth. CLAMP_TO_EDGE on R holds the fully integrated column
    // for anything at or past the last slice.
    vec3 uvw = vec3(TexCoords, (slice - 0.5) / float(froxelDepth));
    FragColor = texture(integratedVolume, uvw);
}
