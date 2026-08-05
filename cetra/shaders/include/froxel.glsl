// Froxel fog volume mapping (spec 9.5). The volume is a camera-frustum grid:
// XY spans the screen, Z spans [near, fogFar] in EXPONENTIAL slices -- the same
// Doom-2016 form the cluster grid uses (see the clusterParams block comment in
// include/lights_ubo.glsl), so a froxel and the light cluster it falls in agree
// about depth.
//
//   z(s) = near * pow(fogFar/near, s / slices)
//   s(z) = log(z/near) / log(fogFar/near) * slices
//
// Slices are CONSTANT-DEPTH PLANES, not shells at constant range: froxelViewPos
// puts every cell of a slice at the same view z. Consumers must therefore index
// by planar depth (-viewPos.z), never by length(viewPos) -- and a path length
// through a slice is the depth step scaled by 1/cos of the ray's angle off the
// optical axis.
//
// The slice count is a parameter, not a constant, so the C side owns it (one
// definition, no C/GLSL mirror to drift) and a second volume at a different
// depth -- the aerial-perspective LUT -- can reuse this mapping unchanged.
//
// Depth range is [near, fogFar], not [near, far]: fog is a short-range effect,
// so spending slices out to the camera far plane would waste almost all of them
// on air the fog never reaches. Slice count is likewise decoupled from CLUSTER_Z
// -- 64 fog slices over a fog-length range resolve near-camera scattering far
// better than 24 over the whole frustum, and the cluster lookup takes a raw
// view Z anyway.

// A degenerate range (fogFar at or inside the near plane, which a small enough
// scene can produce since apps scale fogFar off scene radius) would divide by
// zero here and invert the slice order below it.
float froxelFarZ(float nearZ, float farZ) {
    return max(farZ, nearZ * 1.01);
}

// `dist` biases WHERE the slices bunch, on top of the exponential curve: the
// normalized slice coordinate is raised to it before the exponential is taken.
// 1 is the pure exponential above. Above 1 pulls slices toward the far end,
// below 1 toward the camera. It exists because near and far alone give one
// curve shape, and the depth a scene actually needs resolved is rarely the one
// a pure exponential spends its budget on (UE carries the same knob as
// r.VolumetricFog.DepthDistributionScale).

// Slice index (0 .. slices, continuous) -> positive view depth.
float froxelSliceToViewZ(float slice, float nearZ, float farZ, float slices, float dist) {
    float t = clamp(slice / slices, 0.0, 1.0);
    return nearZ * pow(froxelFarZ(nearZ, farZ) / nearZ, pow(t, dist));
}

// Positive view depth -> continuous slice coordinate. Inverse of the above;
// the caller clamps into [0, slices] as its lookup requires.
float froxelViewZToSlice(float viewZ, float nearZ, float farZ, float slices, float dist) {
    float far1 = froxelFarZ(nearZ, farZ);
    float t = log(max(viewZ, nearZ) / nearZ) / log(far1 / nearZ);
    return pow(max(t, 0.0), 1.0 / max(dist, 1e-3)) * slices;
}

// Sample a front-to-back integrated medium volume at a planar view depth.
//
// Layer s holds the column integrated to the FAR FACE of slice s, so a depth at
// continuous slice coordinate `slice` is carried by layer slice-1, whose texel
// centre sits at (slice-0.5)/slices. CLAMP_TO_EDGE on R holds the fully
// integrated column for anything at or past the last slice.
//
// slices == 0 means the medium is absent, and returns its identity for the
// (inscatter, transmittance) composite: add nothing, attenuate by nothing. That
// keeps "is this medium on" in the data, so a consumer cannot forget to branch.
vec4 froxelSampleMedium(sampler3D vol, vec2 uv, float viewZ, float nearZ, float farZ, int slices,
                        float dist) {
    if (slices == 0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    float slice = froxelViewZToSlice(min(viewZ, farZ), nearZ, farZ, float(slices), dist);
    return texture(vol, vec3(uv, (slice - 0.5) / float(slices)));
}

// The view-space position at the CENTRE of the froxel addressed by (uv, slice),
// where uv is the froxel's screen-space [0,1] coordinate and jitter offsets the
// sample within the slice (0.5 = centre). invFocal is 1/(P[0][0], P[1][1]).
// View space is right-handed with z negative in front of the camera, matching
// include/depth.glsl.
vec3 froxelViewPos(vec2 uv, float slice, float jitter, float nearZ, float farZ, float slices,
                   vec2 invFocal, float dist) {
    float viewZ = froxelSliceToViewZ(slice + jitter, nearZ, farZ, slices, dist);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc * viewZ * invFocal, -viewZ);
}
