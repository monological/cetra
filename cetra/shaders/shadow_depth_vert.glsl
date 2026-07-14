#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 6) in ivec4 aBoneIds;
layout (location = 7) in vec4 aBoneWeights;

#define MAX_BONES 128

uniform mat4 model;
uniform mat4 lightSpaceMatrix;

// Skinning (mirrors pbr_skinned_vert so animated meshes cast animated
// shadows). The skinned flag gates the path because disabled vertex
// attributes read as (0,0,0,1), which would corrupt unskinned meshes.
uniform bool skinned;
uniform mat4 boneMatrices[MAX_BONES];

void main()
{
    vec4 localPos = vec4(aPos, 1.0);

    if (skinned) {
        mat4 boneTransform = mat4(0.0);
        float totalWeight = 0.0;

        for (int i = 0; i < 4; i++) {
            if (aBoneIds[i] >= 0 && aBoneIds[i] < MAX_BONES) {
                boneTransform += boneMatrices[aBoneIds[i]] * aBoneWeights[i];
                totalWeight += aBoneWeights[i];
            }
        }

        if (totalWeight >= 0.001) {
            localPos = boneTransform * vec4(aPos, 1.0);
        }
    }

    gl_Position = lightSpaceMatrix * model * localPos;
}
