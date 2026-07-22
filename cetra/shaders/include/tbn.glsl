// Tangent basis for normal mapping. Both vectors are WORLD space: N is the
// normal this TBN carries, Tworld the tangent already through the model (and,
// for skinned meshes, bone) transform. w is the handedness from the vec4
// tangent attribute (mesh.h: xyz tangent, w = +1, or -1 on mirrored UV
// islands). B is derived rather than stored -- the fragment stage only ever
// took the handedness sign from an authored bitangent, so the stream was
// retired, and this is the one place the cross(N, T) * w convention lives.
//
// N must be the SAME normal the fragment stage orthogonalizes against
// (pbr_frag.glsl re-derives the basis via Gram-Schmidt against the Normal
// varying). Passing a normal from a different transform than that one silently
// tilts the basis relative to the shading normal.
mat3 buildTBN(vec3 N, vec3 Tworld, float w) {
    vec3 T = normalize(Tworld);
    return mat3(T, cross(N, T) * w, N);
}
