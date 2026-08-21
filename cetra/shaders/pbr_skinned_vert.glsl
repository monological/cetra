#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // xyz tangent, w handedness
layout(location = 5) in vec4 aColor;
layout(location = 6) in ivec4 aBoneIds;
layout(location = 7) in vec4 aBoneWeights;
layout(location = 8) in vec2 aTexCoords2;

// centroid, matching pbr_vert and pbr_frag -- the qualifier is part of the interface, so all
// three have to agree or the program fails to link. See pbr_vert for what it prevents.
out vec3 Normal;
out vec3 WorldPos;
out vec3 ViewPos;
out vec2 TexCoords;
out vec2 TexCoords2;
centroid out vec4 VertexColor;
out mat3 TBN;
flat out float TangentW; // bitangent handedness, per-island constant
out vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
out vec4 PrevClip;     // Previous-frame clip position

uniform mat4 model;
// transpose(inverse(model)), uploaded per node (render.c) -- see pbr_vert.
// The BONE inverse-transpose below is different: bone matrices are a per-vertex
// blend, so that one genuinely cannot be hoisted.
uniform mat3 uNormalMatrix;
uniform mat4 view;
uniform mat4 projection; // Jittered when TAA is on (rasterization only)

// Motion-vector inputs (un-jittered), for the per-pixel screen velocity.
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;

uniform float time;
uniform float uDeltaTime; // render clock advance, for the previous-frame position

#include "wind.glsl"
#include "tbn.glsl"

// This stage writes motion vectors, so it takes the previous-pose half too.
#define SKIN_PREV_POSE
#include "skin.glsl"
#include "object_position.glsl"

// See pbr_vert: the depth prepass must reach the same clip position from a
// different program, and GL_LEQUAL rejects anything that lands behind it.
invariant gl_Position;

void main() {
    vec4 localPos;
    vec4 prevLocalPos; // Skinned position under last frame's pose (motion vectors)
    vec3 localNormal;
    vec3 localTangent;
    mat4 boneTransform = mat4(1.0);

    if (skinned) {
        // Current and previous pose in one pass, so the motion vector captures
        // the deformation and not just the node transform.
        boneTransform = skinMatrix(aBoneIds, aBoneWeights);
        mat4 prevBoneTransform = skinMatrixPrev(aBoneIds, aBoneWeights);

        prevLocalPos = prevBoneTransform * vec4(aPos, 1.0);
        mat3 boneRotation = mat3(boneTransform);
        // Normals transform by the inverse-transpose, not the matrix itself.
        // Cross-rig retargeting injects non-uniform scale/shear into the
        // blended bone matrix; mat3(boneTransform) then skews the normal's
        // DIRECTION (normalize only fixes length), which the sharp specular
        // lobe turns into a field of bright specks on the animated mesh.
        // Tangents are surface vectors and use the forward matrix.
        float boneDet = determinant(boneRotation);
        mat3 boneNormalMatrix =
            abs(boneDet) > 1e-8 ? transpose(inverse(boneRotation)) : boneRotation;
        localNormal = boneNormalMatrix * aNormal;
        localTangent = boneRotation * aTangent.xyz;
    } else {
        // Non-skinned: pass through unchanged
        prevLocalPos = vec4(aPos, 1.0);
        localNormal = aNormal;
        localTangent = aTangent.xyz;
    }

    // Posed and wind-displaced, through the chunk the prepass also uses so the
    // two cannot drift. The previous-frame position stays here: it feeds motion
    // vectors, which no depth stage writes.
    // The SAME model matrix for both, because the wind phase is a property of the
    // object rather than of the frame -- taking it from a moved node next frame
    // would report a velocity the raster never drew. The clock and the camera are
    // what differ, and they are the arguments.
    localPos = cetra_local_position(aPos, boneTransform, skinned, aTexCoords, aTexCoords2, time,
                                    model, uMorphEye);
    prevLocalPos.xyz += cetra_local_displacement(aPos, aTexCoords, aTexCoords2,
                                                 time - uDeltaTime, model, uMorphEyePrev);

    CetraObjectPos obj = cetra_object_position(model, view, projection, localPos);
    vec4 worldPos = obj.world;
    WorldPos = worldPos.xyz;

    // Motion vectors (un-jittered): current vs previous skinned position, so
    // the velocity captures camera, node, and per-bone deformation motion.
    CurrClip = uCurrViewProjNoJitter * worldPos;
    PrevClip = uPrevViewProj * uPrevModel * prevLocalPos;

    ViewPos = obj.view.xyz;

    vec4 clipPos = obj.clip;

    Normal = normalize(uNormalMatrix * cetraMorphNormal(localNormal, aPos, model, uMorphEye));
    TexCoords = aTexCoords;
    TexCoords2 = aTexCoords2;
    VertexColor = aColor;

    // Built AFTER the bone and model transforms: a blended bone matrix can
    // carry non-uniform scale, which would shear a transformed bitangent out
    // of square with its own normal and tangent. N is the Normal varying, the
    // one the fragment stage orthogonalizes against.
    TBN = buildTBN(Normal, mat3(model) * localTangent, aTangent.w);
    TangentW = aTangent.w;

    gl_Position = clipPos;
}
