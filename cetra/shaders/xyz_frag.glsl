#version 330 core
in vec3 vertexColor;
out vec4 FragColor;
uniform mat4 view;
uniform mat4 model;
uniform mat4 projection;
void main()
{
    FragColor = vec4(vertexColor, 1.0);
}
