#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // xyz tangent, w handedness
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

// Directional wind (wind.h) -- global scene field + per-material response.
uniform float uDeltaTime;
uniform vec3 uWindDir;
uniform float uWindStrength;
uniform float uWindSpeed;
uniform float uWindGustFreq;
uniform float uWindGustAmount;
uniform float uWindTurbulence;
uniform float uWindResponse;
uniform float uWindMaskMinY;
uniform float uWindMaskMaxY;
uniform int uWindMode;

// World-Position Offset wind (see pbr_vert.glsl for the full note). Cloth is
// masked by object-space height so the hem swings and the top stays pinned;
// the vegetation modes read UV1 for a per-branch phase and a flex weight.
vec3 windOffset(vec3 p, vec2 uv0, vec2 uv1, float t) {
    if (uWindStrength <= 0.0 || uWindResponse <= 0.0)
        return vec3(0.0);

    float gust = mix(1.0 - uWindGustAmount, 1.0, pow(0.5 + 0.5 * sin(t * uWindGustFreq), 3.0));
    vec3 dir = normalize(uWindDir);

    if (uWindMode == 0) {
        float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
        float h = clamp((uWindMaskMaxY - p.y) / denom, 0.0, 1.0);
        float mask = h * h;
        float ph = t * uWindSpeed + p.y * 2.0 + p.x * 1.3;
        float sway = 0.5 + 0.5 * sin(ph);
        float amp = uWindStrength * uWindResponse * mask * gust;
        vec3 flutter = vec3(sin(ph * 3.1), 0.0, cos(ph * 2.7)) * (uWindTurbulence * amp * 0.3);
        return dir * (sway * amp) + flutter;
    }

    float phase = uv1.x * 6.2831853;
    float flex = uv1.y;
    float amp = uWindStrength * uWindResponse * gust;

    float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
    float h = clamp((p.y - uWindMaskMinY) / denom, 0.0, 1.0);
    vec3 off = dir * ((0.5 + 0.5 * sin(t * uWindSpeed * 0.35)) * amp * h * h * 0.6);

    off += dir * (sin(t * uWindSpeed + phase) * amp * flex * 0.5);
    off += vec3(sin(t * uWindSpeed * 1.7 + phase * 2.0), 0.0,
                cos(t * uWindSpeed * 1.3 + phase)) *
           (uWindTurbulence * amp * flex * 0.25);

    if (uWindMode == 2) {
        float f = sin(t * uWindSpeed * 6.0 + phase * 7.0 + p.x * 3.0 + p.z * 2.7);
        off += vec3(f, f * 0.4, -f * 0.6) * (amp * flex * uv0.y * uWindTurbulence);
    }
    return off;
}

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
        // Tangents are surface vectors and use the forward matrix.
        float boneDet = determinant(boneRotation);
        mat3 boneNormalMatrix =
            abs(boneDet) > 1e-8 ? transpose(inverse(boneRotation)) : boneRotation;
        localNormal = boneNormalMatrix * aNormal;
        localTangent = boneRotation * aTangent.xyz;
    } else {
        // Non-skinned: pass through unchanged
        localPos = vec4(aPos, 1.0);
        prevLocalPos = vec4(aPos, 1.0);
        localNormal = aNormal;
        localTangent = aTangent.xyz;
    }

    // Wind (object-space displacement, masked by height). aPos is the un-skinned
    // rest position -- the right reference for the top-pinned/hem-free mask.
    localPos.xyz += windOffset(aPos, aTexCoords, aTexCoords2, time);
    prevLocalPos.xyz += windOffset(aPos, aTexCoords, aTexCoords2, time - uDeltaTime);

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

    // TBN. Deriving the bitangent AFTER the bone and model transforms is also
    // more correct than rotating an authored one: a blended bone matrix can
    // carry non-uniform scale, which shears a transformed bitangent out of
    // square with its own normal and tangent.
    vec3 T = normalize(mat3(model) * localTangent);
    vec3 N = normalize(mat3(model) * localNormal);
    vec3 B = cross(N, T) * aTangent.w;
    TBN = mat3(T, B, N);

    gl_Position = clipPos;
}
