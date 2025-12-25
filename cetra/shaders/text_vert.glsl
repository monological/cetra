#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;
layout(location = 5) in vec4 aColor;

out vec2 TexCoord;
out vec4 Color;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform int isScreenSpace;

void main() {
    TexCoord = aTexCoord;
    Color = aColor;

    if (isScreenSpace == 1) {
        gl_Position = projection * model * vec4(aPos, 1.0);
    } else {
        gl_Position = projection * view * model * vec4(aPos, 1.0);
    }
}
