// Per-instance transforms, for the draws that carry more than one object.
//
// The three values a draw needs per object -- model, its previous-frame twin
// for motion vectors, and the normal matrix -- as a std140 array indexed by
// gl_InstanceID. The C side owns the layout and validates it against the
// driver's reported block size (ubo.h), so a mismatch reports rather than
// silently reading the wrong floats.
//
// Attribute divisors were the alternative and do not fit: the mesh VAOs have
// four free slots between them, these three values need eleven, and packing to
// fit means deriving the normal matrix in the shader -- which would make the
// instanced and non-instanced paths differ in the last bits and give up the one
// arm that can prove they agree. Divisors would also have to mutate the mesh
// VAO, which the depth pass and the non-instanced path share.
//
// One program, not two. uInstanced is uniform across a whole draw, so every
// driver specialises the branch, where a #define'd variant costs a program
// switch per batch and a second copy of the vertex path that can drift.

#define CETRA_INSTANCE_MAX 64

layout(std140) uniform InstanceBlock {
    mat4 uInstModel[CETRA_INSTANCE_MAX];
    mat4 uInstPrevModel[CETRA_INSTANCE_MAX];
    // The normal matrix as a full mat4; the shader reads its upper 3x3. std140
    // pads a mat3 to 48 bytes anyway, so this costs 16 bytes an instance and
    // removes a class of column-packing bugs.
    mat4 uInstNormal[CETRA_INSTANCE_MAX];
};

uniform bool uInstanced;

// gl_InstanceID is 0 for a non-instanced draw, so both arms are always safe to
// evaluate; the select is what keeps the downstream arithmetic textually
// identical between them.
mat4 cetra_instance_model(mat4 fallback) {
    return uInstanced ? uInstModel[gl_InstanceID] : fallback;
}

mat4 cetra_instance_prev_model(mat4 fallback) {
    return uInstanced ? uInstPrevModel[gl_InstanceID] : fallback;
}

mat3 cetra_instance_normal_matrix(mat3 fallback) {
    return uInstanced ? mat3(uInstNormal[gl_InstanceID]) : fallback;
}
