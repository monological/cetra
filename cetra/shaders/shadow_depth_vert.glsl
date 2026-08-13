#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoords;
layout (location = 5) in vec4 aColor;
layout (location = 6) in ivec4 aBoneIds;
layout (location = 7) in vec4 aBoneWeights;
layout (location = 8) in vec2 aTexCoords2;

out vec2 TexCoords; // for the alpha test on foliage (material.h foliage_shadows)
// Vertex-colour alpha multiplies into the cutout the same way it does when the
// surface is shaded. A caster whose alpha comes from COLOR_0 rather than from
// its albedo map used to cast as if it were solid.
out vec4 VertexColor;

uniform mat4 model;
uniform mat4 lightSpaceMatrix;
uniform float time;

// No SKIN_PREV_POSE: the depth pass writes no motion vectors, so it skips
// uPrevBoneRows and the vertex uniform budget that comes with it.
#include "skin.glsl"
#include "wind.glsl" // must match the shading passes exactly -- see the chunk
#include "instancing.glsl"
#include "object_position.glsl"

void main()
{
    // Same posing and wind as the shading passes, from the same chunk. This
    // stage keeps its own final multiply rather than taking
    // cetra_object_position: a light has one lightSpaceMatrix where the camera
    // splits view and projection, and this pass renders under a polygon offset
    // and is read through a bias, so it has no bit-exactness to preserve. The
    // DEPTH PREPASS is the stage that does -- see object_position.glsl.
    vec4 localPos = cetra_local_position(aPos, skinMatrixOrIdentity(aBoneIds, aBoneWeights),
                                         skinned, aTexCoords, aTexCoords2, time);

    TexCoords = aTexCoords;
    VertexColor = aColor;
    gl_Position = lightSpaceMatrix * cetra_instance_model(model) * localPos;
}
