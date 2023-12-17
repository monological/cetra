#version 410 core
layout(location = 0) in vec3 aPos;   // Position of the vertex
layout(location = 1) in vec3 aColor; // Color of the vertex

out vec3 vertexColor; // Variable to transfer color to geometry shader
out vec3 WorldPos;     // World position

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform vec3 camPos;
uniform float time;

void main()
{
    vertexColor = aColor;          // Pass the color to the next shader stage

    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos = worldPos.xyz;

    vec4 viewPos = view * worldPos;
    vec4 clipPos = projection * viewPos;
    gl_Position = clipPos;
}

