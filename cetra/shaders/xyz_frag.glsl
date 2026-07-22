#version 330 core
in vec3 vertexColor;
out vec4 FragColor;

// No transform uniforms here: xyz_vert does the projection, this stage only
// writes the interpolated axis colour.
void main()
{
    FragColor = vec4(vertexColor, 1.0);
}
