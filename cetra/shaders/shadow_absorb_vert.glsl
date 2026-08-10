#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoords;
layout (location = 6) in ivec4 aBoneIds;
layout (location = 7) in vec4 aBoneWeights;
layout (location = 8) in vec2 aTexCoords2;

out vec2 TexCoords;

uniform mat4 model;
uniform mat4 lightSpaceMatrix;
uniform float time;

// Skinning and wind must displace an absorbance caster EXACTLY as they displace
// the same surface in the depth pass and the shading pass. This file is a
// deliberate mirror of shadow_depth_vert.glsl for that reason: a groom whose
// shadow lags the geometry it comes from is worse than no shadow, and the two
// would drift silently because nothing compares them.
#include "skin.glsl"
#include "wind.glsl"

void main()
{
    vec4 localPos = vec4(aPos, 1.0);

    if (skinned) {
        localPos = skinMatrix(aBoneIds, aBoneWeights) * vec4(aPos, 1.0);
    }

    localPos.xyz += windOffset(aPos, aTexCoords, aTexCoords2, time);

    TexCoords = aTexCoords;
    gl_Position = lightSpaceMatrix * model * localPos;
}
