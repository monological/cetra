#version 330 core
layout (triangles) in;
layout (triangle_strip, max_vertices = 3) out;
in vec3 vNormal[];
in vec2 vTexCoord[];
out vec3 gNormal;
out vec2 gTexCoord;
void main() {
    gNormal = vNormal[0];
    gTexCoord = vTexCoord[0];
    gl_Position = gl_in[0].gl_Position + vec4(-0.1, 0.0, 0.0, 0.0);
    EmitVertex();
    gl_Position = gl_in[0].gl_Position + vec4( 0.1, 0.0, 0.0, 0.0);
    EmitVertex();
    EndPrimitive();
};

