#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;
layout(location = 5) in vec4 aColor;
layout(location = 6) in ivec4 aBoneIds;
layout(location = 7) in vec4 aBoneWeights;
layout(location = 8) in vec2 aTexCoords2;

out vec3 Normal;
out vec3 WorldPos;
out vec3 ViewPos;
out vec3 FragPos;
out float ClipDepth;
out float FragDepth;
out vec2 TexCoords;
out vec2 TexCoords2;
out vec4 VertexColor;
out mat3 TBN;
out vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
out vec4 PrevClip;     // Previous-frame clip position

#define MAX_BONES 128

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection; // Jittered when TAA is on (rasterization only)

// Motion-vector inputs (un-jittered), for the per-pixel screen velocity.
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;

uniform vec3 camPos;
uniform float time;

// Skinning uniforms
uniform bool skinned;
uniform mat4 boneMatrices[MAX_BONES];
// Previous frame's bones for motion vectors, packed as 3 affine rows per bone
// (the implicit 4th row is 0,0,0,1) so a second full set fits the vertex
// uniform budget alongside boneMatrices.
uniform vec4 uPrevBoneRows[3 * MAX_BONES];

mat4 prevBone(int i) {
    vec4 r0 = uPrevBoneRows[3 * i + 0];
    vec4 r1 = uPrevBoneRows[3 * i + 1];
    vec4 r2 = uPrevBoneRows[3 * i + 2];
    return mat4(r0.x, r1.x, r2.x, 0.0,
                r0.y, r1.y, r2.y, 0.0,
                r0.z, r1.z, r2.z, 0.0,
                r0.w, r1.w, r2.w, 1.0);
}

void main() {
    vec4 localPos;
    vec4 prevLocalPos; // Skinned position under last frame's pose (motion vectors)
    vec3 localNormal;
    vec3 localTangent;
    vec3 localBitangent;

    if (skinned) {
        // Apply bone transforms weighted by bone weights (current and previous
        // pose in one pass so the motion vector captures the deformation).
        mat4 boneTransform = mat4(0.0);
        mat4 prevBoneTransform = mat4(0.0);
        float totalWeight = 0.0;

        for (int i = 0; i < 4; i++) {
            if (aBoneIds[i] >= 0 && aBoneIds[i] < MAX_BONES) {
                boneTransform += boneMatrices[aBoneIds[i]] * aBoneWeights[i];
                prevBoneTransform += prevBone(aBoneIds[i]) * aBoneWeights[i];
                totalWeight += aBoneWeights[i];
            }
        }

        // Fallback to identity if no valid bones
        if (totalWeight < 0.001) {
            boneTransform = mat4(1.0);
            prevBoneTransform = mat4(1.0);
        }

        // Transform position and normals by bone matrix
        localPos = boneTransform * vec4(aPos, 1.0);
        prevLocalPos = prevBoneTransform * vec4(aPos, 1.0);
        mat3 boneRotation = mat3(boneTransform);
        // Normals transform by the inverse-transpose, not the matrix itself.
        // Cross-rig retargeting injects non-uniform scale/shear into the
        // blended bone matrix; mat3(boneTransform) then skews the normal's
        // DIRECTION (normalize only fixes length), which the sharp specular
        // lobe turns into a field of bright specks on the animated mesh.
        // Tangents/bitangents are surface vectors and use the forward matrix.
        float boneDet = determinant(boneRotation);
        mat3 boneNormalMatrix =
            abs(boneDet) > 1e-8 ? transpose(inverse(boneRotation)) : boneRotation;
        localNormal = boneNormalMatrix * aNormal;
        localTangent = boneRotation * aTangent;
        localBitangent = boneRotation * aBitangent;
    } else {
        // Non-skinned: pass through unchanged
        localPos = vec4(aPos, 1.0);
        prevLocalPos = vec4(aPos, 1.0);
        localNormal = aNormal;
        localTangent = aTangent;
        localBitangent = aBitangent;
    }

    // Transform to world space
    vec4 worldPos = model * localPos;
    WorldPos = worldPos.xyz;

    // Motion vectors (un-jittered): current vs previous skinned position, so
    // the velocity captures camera, node, and per-bone deformation motion.
    CurrClip = uCurrViewProjNoJitter * worldPos;
    PrevClip = uPrevViewProj * uPrevModel * prevLocalPos;

    vec4 viewPos = view * worldPos;
    ViewPos = viewPos.xyz;

    vec4 clipPos = projection * viewPos;
    FragPos = clipPos.xyz;
    ClipDepth = clipPos.z;

    FragDepth = clipPos.z / clipPos.w;

    // Transform normals to world space
    mat3 normalMatrix = mat3(transpose(inverse(model)));
    Normal = normalize(normalMatrix * localNormal);
    TexCoords = aTexCoords;
    TexCoords2 = aTexCoords2;
    VertexColor = aColor;

    // Calculate TBN matrix for normal mapping
    vec3 T = normalize(mat3(model) * localTangent);
    vec3 B = normalize(mat3(model) * localBitangent);
    vec3 N = normalize(mat3(model) * localNormal);
    TBN = mat3(T, B, N);

    gl_Position = clipPos;
}
