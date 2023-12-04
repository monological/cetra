#version 330 core
in vec3 vertexColor;

out vec4 FragColor;

uniform vec3 view;
uniform vec3 model;
uniform vec3 projection;

void main()
{
    FragColor = vec4(vertexColor, 1.0);
}

