#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;
layout(location = 6) in ivec4 aBoneIds;
layout(location = 7) in vec4 aBoneWeights;

out vec3 Normal;
out vec3 WorldPos;
out vec3 ViewPos;
out vec3 FragPos;
out float ClipDepth;
out float FragDepth;
out vec2 TexCoords;
out mat3 TBN;

#define MAX_LIGHTS 70
#define MAX_BONES  128

struct Light {
    int type;
    vec3 position;
    vec3 direction;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
    vec2 size;
};

uniform Light lights[MAX_LIGHTS];
uniform int numLights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform vec3 camPos;
uniform float time;

// Skinning uniforms
uniform bool skinned;
uniform mat4 boneMatrices[MAX_BONES];

void main() {
    vec4 localPos;
    vec3 localNormal;
    vec3 localTangent;
    vec3 localBitangent;

    if (skinned) {
        // Apply bone transforms weighted by bone weights
        mat4 boneTransform = mat4(0.0);
        float totalWeight = 0.0;

        for (int i = 0; i < 4; i++) {
            if (aBoneIds[i] >= 0 && aBoneIds[i] < MAX_BONES) {
                boneTransform += boneMatrices[aBoneIds[i]] * aBoneWeights[i];
                totalWeight += aBoneWeights[i];
            }
        }

        // Fallback to identity if no valid bones
        if (totalWeight < 0.001) {
            boneTransform = mat4(1.0);
        }

        // Transform position and normals by bone matrix
        localPos = boneTransform * vec4(aPos, 1.0);
        mat3 boneRotation = mat3(boneTransform);
        localNormal = boneRotation * aNormal;
        localTangent = boneRotation * aTangent;
        localBitangent = boneRotation * aBitangent;
    } else {
        // Non-skinned: pass through unchanged
        localPos = vec4(aPos, 1.0);
        localNormal = aNormal;
        localTangent = aTangent;
        localBitangent = aBitangent;
    }

    // Transform to world space
    vec4 worldPos = model * localPos;
    WorldPos = worldPos.xyz;

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

    // Calculate TBN matrix for normal mapping
    vec3 T = normalize(mat3(model) * localTangent);
    vec3 B = normalize(mat3(model) * localBitangent);
    vec3 N = normalize(mat3(model) * localNormal);
    TBN = mat3(T, B, N);

    gl_Position = clipPos;
}
