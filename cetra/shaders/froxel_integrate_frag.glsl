#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = accumulated in-scatter, a = transmittance to this slice

// Froxel fog, pass 2 of 3 (spec 9.5): march front-to-back along each froxel
// column so the composite becomes a single trilinear tap.
//
// Slice k gathers slices 0..k from the scatter volume: O(n^2) in slice count,
// ~30M texel fetches at 64 slices, where the accumulation itself is O(n).
//
// Rejected alternatives, in order of how good they actually are:
//   - Sequential scan writing into the volume it reads: needs glTextureBarrier
//     (GL 4.5) to be defined. Not available.
//   - Ping-pong the running (L, T) through a pair of 160x90 RGBA16F 2D targets,
//     one extra attachment per draw. Same 64 draws, ~0.9M fetches, and it
//     deletes this loop -- the codebase's own accumulator idiom (PingPong).
//     This is the lever to pull first if the pass ever profiles hot.
//   - MRT layer blocks: 8 layers per draw as 8 attachments, ~4.2M fetches and
//     8 draws, and it recovers the per-column hoists one-cell-per-invocation
//     forbids. Better than the scan on draw count, worse on fetches.
// The gather ships because it is the simplest correct form, not because it is
// the cheapest; no frame timing has been measured either way.

#include "froxel.glsl"

uniform sampler3D scatterVolume;
uniform int sliceIndex;
uniform mat4 projection;
uniform float fogFar;
uniform int froxelDepth; // Slice count; mirrors POSTFX_FROXEL_Z

void main() {
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);

    // Slices are constant-DEPTH planes, but extinction integrates along the
    // ray, which is longer than the depth step by 1/cos of the ray's angle off
    // the optical axis -- 1.38x at the corner of a 50-degree 16:9 frame. Without
    // this the frame edges under-extinguish.
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    vec2 ndc = (gl_FragCoord.xy / vec2(textureSize(scatterVolume, 0).xy)) * 2.0 - 1.0;
    float rayScale = length(vec3(ndc * invFocal, 1.0));

    // Consecutive slice depths differ by a constant ratio (the mapping is
    // exponential), so the walk is one multiply per step instead of a pow.
    float sliceRatio = pow(froxelFarZ(nearZ, fogFar) / nearZ, 1.0 / float(froxelDepth));

    vec3 L = vec3(0.0);
    float T = 1.0;
    float prevZ = nearZ;
    for (int s = 0; s <= sliceIndex; s++) {
        float z = prevZ * sliceRatio;
        float dt = (z - prevZ) * rayScale;
        prevZ = z;

        vec4 cell = texelFetch(scatterVolume, ivec3(gl_FragCoord.xy, s), 0);
        // Energy-conserving segment integral (the 4.5 form): the analytic
        // integral of S*sigma*exp(-sigma x) over the step, stable for any
        // sigma*dt where a plain Riemann sum overshoots at scene scale.
        float stepTrans = exp(-cell.a * dt);
        L += T * (1.0 - stepTrans) * cell.rgb;
        T *= stepTrans;
        if (T < 0.003) { // the march's early-out: the rest cannot contribute
            T = 0.0;
            break;
        }
    }

    FragColor = vec4(min(L, vec3(500.0)), T);
}
