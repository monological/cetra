#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = accumulated in-scatter, a = transmittance to this slice

// Froxel fog, pass 2 of 3 (spec 9.5): march front-to-back along each froxel
// column so the composite becomes a single trilinear tap.
//
// Slice k gathers slices 0..k from the scatter volume. That is O(n^2) in slice
// count (~30M taps at 64 slices) where a sequential scan would be O(n) -- but a
// sequential scan has to read the slice it just wrote, and GL 4.1 has no
// glTextureBarrier (4.5) to make that defined. Reading one volume and writing a
// different one has no hazard at all, so the gather is the honest way to do it
// here. (The (scatter, transmittance) operator is associative, so a 6-pass
// Hillis-Steele scan would cut this to ~5.5M taps at 6x the draw calls -- the
// lever if this ever profiles hot.)

#include "froxel.glsl"

uniform sampler3D scatterVolume;
uniform int sliceIndex;
uniform mat4 projection;
uniform float fogFar;

void main() {
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);

    vec3 L = vec3(0.0);
    float T = 1.0;
    float prevZ = nearZ;
    for (int s = 0; s <= sliceIndex; s++) {
        // Slice thickness in world units: the exponential mapping makes far
        // slices much longer than near ones, and the extinction integral is
        // over distance, so dt must come from the mapping, not be assumed flat.
        float z = froxelSliceToViewZ(float(s) + 1.0, nearZ, fogFar);
        float dt = z - prevZ;
        prevZ = z;

        vec4 cell = texelFetch(scatterVolume, ivec3(gl_FragCoord.xy, s), 0);
        // Energy-conserving segment integral (the 4.5 form): the analytic
        // integral of S*sigma*exp(-sigma x) over the step, stable for any
        // sigma*dt where a plain Riemann sum overshoots at scene scale.
        float stepTrans = exp(-cell.a * dt);
        L += T * (1.0 - stepTrans) * cell.rgb;
        T *= stepTrans;
    }

    FragColor = vec4(min(L, vec3(500.0)), T);
}
