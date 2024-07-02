#version 410 core
in vec3 geomColor; // The color passed from the geometry shader
out vec4 FragColor; // Output color of the pixel

void main()
{
    FragColor = vec4(geomColor, 1.0); // Set the output color of the pixel
}

