#version 330 core
layout(location = 0) in vec3 aPos;

out vec3 WorldPos;

uniform mat4 view;
uniform mat4 projection;
uniform float planeRadius;

void main()
{
    // Unit quad at y=0 scaled to the catcher radius in world space
    WorldPos = vec3(aPos.x * planeRadius, 0.0, aPos.z * planeRadius);
    gl_Position = projection * view * vec4(WorldPos, 1.0);
}
