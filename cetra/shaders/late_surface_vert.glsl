#version 330 core

// The vertex stage of a material drawn in the late draw (spec 13.29), under an app's fragment
// stage. One object a draw, with the projection TAA did not jitter: the late draw is past the
// temporal seam, and a jittered surface there would shimmer by the jitter with nothing to
// resolve it.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 5) in vec4 aColor;

uniform mat4 model;
uniform mat3 uNormalMatrix;
uniform mat4 view;
uniform mat4 projection;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
out float vViewDepth;

void main()
{
    vec4 world = model * vec4(aPos, 1.0);
    vec4 eye = view * world;
    vWorldPos = world.xyz;
    vNormal = normalize(uNormalMatrix * aNormal);
    vUv = aTexCoords;
    vColor = aColor;
    vViewDepth = -eye.z;
    gl_Position = projection * eye;
}
