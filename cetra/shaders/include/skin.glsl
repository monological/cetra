// Linear-blend skinning, shared by the shading pass (pbr_skinned_vert) and the
// shadow depth pass. shadow_depth_vert's own comment used to say it "mirrors
// pbr_skinned_vert" -- the same hand-kept-in-step arrangement the wind chunk
// replaced, in the same pair of files, and it had already drifted in form (the
// two wrote the degenerate-weight guard inverted relative to each other).
// Both displace the same vertex, so a divergence puts a shadow where its
// caster isn't.
//
// The uniform names here are load-bearing: render_update_skinning_uniforms
// (render.c) uploads `skinned`, `boneMatrices` and `uPrevBoneRows` by literal
// string, for BOTH the scene draw and the shadow pass. Renaming any of them
// fails silently -- uniform_set_* no-ops on a location of -1.
//
// Define SKIN_PREV_POSE before including to also get the previous-frame pose
// (motion vectors). The depth pass deliberately does NOT: it has no motion
// vectors to write, and uPrevBoneRows is 384 vec4s of vertex uniform budget
// that pbr_skinned_vert already has to pack 3-rows-per-bone to afford.

#define MAX_BONES 128

// The flag gates the path because disabled vertex attributes read as
// (0,0,0,1), which would corrupt unskinned meshes.
uniform bool skinned;
uniform mat4 boneMatrices[MAX_BONES];

// Blended bone transform for this vertex; identity when the weights are
// degenerate. Returns identity for unskinned meshes too, so callers can apply
// it unconditionally once they have checked `skinned`.
mat4 skinMatrix(ivec4 ids, vec4 weights)
{
    mat4 m = mat4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; i++) {
        if (ids[i] >= 0 && ids[i] < MAX_BONES) {
            m += boneMatrices[ids[i]] * weights[i];
            total += weights[i];
        }
    }
    return total < 0.001 ? mat4(1.0) : m;
}

#ifdef SKIN_PREV_POSE
// Previous frame's bones for motion vectors, packed as 3 affine rows per bone
// (the implicit 4th row is 0,0,0,1) so a second full set fits the vertex
// uniform budget alongside boneMatrices.
uniform vec4 uPrevBoneRows[3 * MAX_BONES];

mat4 prevBone(int i)
{
    vec4 r0 = uPrevBoneRows[3 * i + 0];
    vec4 r1 = uPrevBoneRows[3 * i + 1];
    vec4 r2 = uPrevBoneRows[3 * i + 2];
    return mat4(r0.x, r1.x, r2.x, 0.0,
                r0.y, r1.y, r2.y, 0.0,
                r0.z, r1.z, r2.z, 0.0,
                r0.w, r1.w, r2.w, 1.0);
}

mat4 skinMatrixPrev(ivec4 ids, vec4 weights)
{
    mat4 m = mat4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; i++) {
        if (ids[i] >= 0 && ids[i] < MAX_BONES) {
            m += prevBone(ids[i]) * weights[i];
            total += weights[i];
        }
    }
    return total < 0.001 ? mat4(1.0) : m;
}
#endif
