#version 330 core

// The backpack's turntable (spec 13.40): one item drawn alone, off the world, in a light of its
// own. The mesh's own attributes, at the engine's locations.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 5) in vec4 aColor;

uniform mat4 model; // a turn and a translation: the normal takes its upper 3x3 as it is
uniform mat4 viewProj;

out vec3 vWorld;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;

void main()
{
    vec4 world = model * vec4(aPosition, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(model) * aNormal;
    vUv = aUv;
    vColor = aColor;
    gl_Position = viewProj * world;
}
