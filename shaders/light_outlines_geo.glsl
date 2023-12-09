#version 330 core
layout (points) in;
layout (line_strip, max_vertices = 100) out;

in vec3 vertexColor[];
in vec3 WorldPos[];

out vec3 geomColor;

#define MAX_LIGHTS 10

struct Light {
    int type;
    vec3 position;
    vec3 direction;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
    vec2 size;
};


uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 camPos;
uniform float time;

uniform Light lights[MAX_LIGHTS];
uniform int numLights;

const int numSegments = 16; // For the sphere approximation
const float PI = 3.14159265359;

void EmitSphere(vec3 center, float radius) {
    for (int i = 0; i <= numSegments; i++) {
        float angle = 2.0 * PI * float(i) / float(numSegments);
        float x = radius * cos(angle);
        float y = radius * sin(angle);
        vec3 pos = center + vec3(x, y, 0.0); // Circle in the XY plane
        gl_Position = projection * view * vec4(pos, 1.0);
        EmitVertex();
        if (i == numSegments) {
            EndPrimitive();
        }
    }
}

void EmitRectangle(vec3 center, vec3 direction, float width, float height) {
    // Calculate the perpendicular right and up vectors
    vec3 right = normalize(cross(direction, vec3(0.0, 1.0, 0.0))) * width;
    vec3 up = normalize(cross(right, direction)) * height;

    // Calculate the four corners of the rectangle
    vec3 corner1 = center + right + up;
    vec3 corner2 = center + right - up;
    vec3 corner3 = center - right - up;
    vec3 corner4 = center - right + up;

    // Emit the four corners of the rectangle
    gl_Position = projection * view * vec4(corner1, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(corner2, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(corner3, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(corner4, 1.0);
    EmitVertex();
    gl_Position = projection * view * vec4(corner1, 1.0); // Close the loop
    EmitVertex();
    EndPrimitive();
}


void main() {
    geomColor = vertexColor[0];
    EmitSphere(vec3(0.0, 1.0, 0.0), 200.0);
    for (int i = 0; i < numLights; i++) {
        vec3 lightPos = WorldPos[0] + lights[i].position; // Offset by vertex position

        if (lights[i].type == 0) { // Point light
            EmitSphere(lightPos, 20.0); // Small sphere for point light
        } else if (lights[i].type == 1 || lights[i].type == 2) { // Directional or Spot light
            EmitRectangle(lightPos, lights[i].direction, 20.0, 15.0); // Rectangle for directional/spot light
        }
    }
}

