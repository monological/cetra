#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoords;
layout(location = 6) in ivec4 aBoneIds;
layout(location = 7) in vec4 aBoneWeights;
layout(location = 8) in vec2 aTexCoords2;

// A view's outlined surfaces (spec 14.39), into its outline mask once its picture is made: every
// surface the view draws whose tag has an outline, and nothing else. The depth prepass's position,
// from the same chunk under the same qualifier, so a surface reaches the depth that pass wrote for
// it and the mask can say where it is seen.

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection; // unjittered: the outline is drawn over an average of jittered frames
uniform float time;

#include "skin.glsl"
#include "wind.glsl"
#include "instancing.glsl"
#include "object_position.glsl"
#include "tags.glsl"

flat out vec4 OutlineColour; // display sRGB, alpha its strength

invariant gl_Position;

void main()
{
    mat4 mModel = cetra_instance_model(model);
    vec4 localPos = cetra_local_position(aPos, skinMatrixOrIdentity(aBoneIds, aBoneWeights),
                                         skinned, aTexCoords, aTexCoords2, time, mModel, uMorphEye);
    vec4 clip = cetra_object_position(mModel, view, projection, localPos).clip;
    CetraTag tag = cetraTag(cetra_instance_tag(uNodeTag));
    OutlineColour = cetraTagOutline(tag.object);
    gl_Position = tag.drop || OutlineColour.a <= 0.0 ? CETRA_TAG_DROPPED : clip;
}
