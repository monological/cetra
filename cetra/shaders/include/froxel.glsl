// Froxel fog volume mapping (spec 9.5). The volume is a camera-frustum grid:
// XY spans the screen, Z spans [near, fogFar] in EXPONENTIAL slices -- the same
// Doom-2016 form the cluster grid uses (include/lights_ubo.glsl:8-12), so a
// froxel and the light cluster it falls in agree about depth.
//
//   z(s) = near * pow(fogFar/near, s / FROXEL_Z)
//   s(z) = log(z/near) / log(fogFar/near) * FROXEL_Z
//
// Slices are deliberately NOT tied to CLUSTER_Z: 64 fog slices over a
// fog-length range resolve near-camera scattering far better than 24 over the
// whole camera far plane, and the cluster lookup takes a raw view Z anyway.
//
// Depth range is [near, fogFar], not [near, far]: fog is a short-range effect,
// so spending slices out to the camera far plane would waste almost all of them
// on air the fog never reaches.

const int FROXEL_Z = 64;

// Slice index (0 .. FROXEL_Z, continuous) -> positive view depth.
float froxelSliceToViewZ(float slice, float nearZ, float farZ) {
    return nearZ * pow(farZ / nearZ, slice / float(FROXEL_Z));
}

// Positive view depth -> continuous slice coordinate. Inverse of the above;
// the caller clamps into [0, FROXEL_Z] as its lookup requires.
float froxelViewZToSlice(float viewZ, float nearZ, float farZ) {
    return log(max(viewZ, nearZ) / nearZ) / log(farZ / nearZ) * float(FROXEL_Z);
}

// The view-space position at the CENTRE of the froxel addressed by (uv, slice),
// where uv is the froxel's screen-space [0,1] coordinate and jitter offsets the
// sample within the slice (0.5 = centre). invFocal is 1/(P[0][0], P[1][1]).
// View space is right-handed with z negative in front of the camera, matching
// include/depth.glsl.
vec3 froxelViewPos(vec2 uv, float slice, float jitter, float nearZ, float farZ, vec2 invFocal) {
    float viewZ = froxelSliceToViewZ(slice + jitter, nearZ, farZ);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc * viewZ * invFocal, -viewZ);
}
