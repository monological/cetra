#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 6) out;

in vec3 WorldPos_vs[2]; // World position from vertex shader

uniform mat4 projection;
uniform mat4 view;
uniform float lineWidth;
uniform float time;

void main() {
    vec3 startPosition = WorldPos_vs[0];
    vec3 endPosition = WorldPos_vs[1];

    // Direction and perpendicular vector
    vec3 lineDir = normalize(endPosition - startPosition);
    vec3 perpVec = normalize(vec3(-lineDir.y, lineDir.x, 0.0)) * lineWidth * 0.5;

    // Offset vector: a small fraction of the line direction
    vec3 offsetVec = lineDir * 0.12 * lineWidth; // Adjust this value as needed

    gl_Position = projection * view * vec4(startPosition + perpVec - offsetVec, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(startPosition - perpVec - offsetVec, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(endPosition + perpVec + offsetVec, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(endPosition - perpVec + offsetVec, 1.0);
    EmitVertex();

    EndPrimitive();
}


