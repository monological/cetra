#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoords;
layout (location = 6) in ivec4 aBoneIds;
layout (location = 7) in vec4 aBoneWeights;
layout (location = 8) in vec2 aTexCoords2;

out vec2 TexCoords; // for the alpha test on foliage (material.h foliage_shadows)

uniform mat4 model;
uniform mat4 lightSpaceMatrix;
uniform float time;

// No SKIN_PREV_POSE: the depth pass writes no motion vectors, so it skips
// uPrevBoneRows and the vertex uniform budget that comes with it.
#include "skin.glsl"
#include "wind.glsl" // must match the shading passes exactly -- see the chunk
#include "instancing.glsl"

void main()
{
    vec4 localPos = vec4(aPos, 1.0);

    if (skinned) {
        localPos = skinMatrix(aBoneIds, aBoneWeights) * vec4(aPos, 1.0);
    }

    localPos.xyz += windOffset(aPos, aTexCoords, aTexCoords2, time);

    TexCoords = aTexCoords;
    gl_Position = lightSpaceMatrix * cetra_instance_model(model) * localPos;
}
