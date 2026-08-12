#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoords;
layout(location = 6) in ivec4 aBoneIds;
layout(location = 7) in vec4 aBoneWeights;
layout(location = 8) in vec2 aTexCoords2;

// The whole point of a separate program rather than reusing pbr: this stage
// emits gl_Position and nothing else. pbr_vert additionally computes windOffset
// a SECOND time for the previous frame, two more clip positions for motion
// vectors, a view position and a TBN basis, then interpolates eight varyings a
// depth pass would throw away -- on the one pass whose entire justification is
// being cheaper than the pass it shields.

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection; // the SAME jittered matrix the shading pass rasterizes with
uniform float time;

#include "skin.glsl"
#include "wind.glsl"
#include "instancing.glsl"
#include "object_position.glsl"

// Load-bearing, not decoration. The shading pass tests against this depth with
// GL_LEQUAL, and a driver free to schedule the same arithmetic differently in
// two programs would put the shading fragment marginally behind the depth it
// just wrote -- which fails the test and deletes the surface.
invariant gl_Position;

void main()
{
    // Branch kept rather than folded into the call: skinMatrix blends four
    // bones, and as an argument it would run for every rigid vertex too.
    mat4 bone = mat4(1.0);
    if (skinned)
        bone = skinMatrix(aBoneIds, aBoneWeights);

    vec4 localPos = cetra_local_position(aPos, bone, skinned, aTexCoords, aTexCoords2, time);
    gl_Position =
        cetra_object_position(cetra_instance_model(model), view, projection, localPos).clip;
}
