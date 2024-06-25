#version 330 core
layout (points) in;
layout (triangle_strip, max_vertices = 128) out;

in vec3 vertexColor[];
in vec3 WorldPos[];

out vec3 geomColor;

#define MAX_LIGHTS 50

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

const float PI = 3.14159265359;

void EmitSphere(vec3 center, float radius) {
    const int numSegments = 12;
    for (int phiIdx = 0; phiIdx <= numSegments; ++phiIdx) {
        // 'phi' ranges from 0 to Pi (from top to bottom)
        float phi = PI * float(phiIdx) / float(numSegments);
        float sinPhi = sin(phi);
        float cosPhi = cos(phi);

        for (int thetaIdx = 0; thetaIdx <= numSegments; ++thetaIdx) {
            // 'theta' ranges from 0 to 2*Pi (around the sphere)
            float theta = 2.0 * PI * float(thetaIdx) / float(numSegments);
            float sinTheta = sin(theta);
            float cosTheta = cos(theta);

            // Compute the vertex position on the surface of the sphere
            vec3 pos;
            pos.x = cosTheta * sinPhi;
            pos.y = cosPhi;
            pos.z = sinTheta * sinPhi;
            pos = pos * radius + center;

            gl_Position = projection * view * vec4(pos, 1.0);
            EmitVertex();

            // Complete the primitive every numSegments+1 vertices
            if (thetaIdx == numSegments) {
                EndPrimitive();
            }
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

void EmitCone(vec3 apex, vec3 direction, float cutOff, float outerCutOff, float length) {
    vec3 axis = normalize(direction);
    float baseRadius = tan(outerCutOff) * length;
    vec3 baseCenter = apex + axis * length;
    const int numSegments = 12;

    // Emit the base as a triangle fan
    gl_Position = projection * view * vec4(baseCenter, 1.0);
    EmitVertex();
    for (int i = 0; i <= numSegments; ++i) {
        float theta = 2.0 * PI * float(i) / float(numSegments);
        vec3 circlePoint = baseCenter + (cos(theta) * baseRadius * normalize(cross(axis, vec3(0.0, 1.0, 0.0))) + sin(theta) * baseRadius * normalize(cross(axis, cross(axis, vec3(0.0, 1.0, 0.0)))));
        gl_Position = projection * view * vec4(circlePoint, 1.0);
        EmitVertex();
    }
    EndPrimitive();

    // Emit the sides as a triangle strip
    for (int i = 0; i <= numSegments; ++i) {
        float theta = 2.0 * PI * float(i) / float(numSegments);
        vec3 circlePoint = baseCenter + (cos(theta) * baseRadius * normalize(cross(axis, vec3(0.0, 1.0, 0.0))) + sin(theta) * baseRadius * normalize(cross(axis, cross(axis, vec3(0.0, 1.0, 0.0)))));
        gl_Position = projection * view * vec4(circlePoint, 1.0);
        EmitVertex();
        gl_Position = projection * view * vec4(apex, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}




void main() {
    for (int i = 0; i < numLights; i++) {
        vec3 lightPos = lights[i].position;

        if (lights[i].type == 0) {
            geomColor = vec3(1.0, 0.0, 0.0);
            EmitSphere(lightPos, 15.0);
        } else if (lights[i].type == 1) {
            geomColor = vec3(0.0, 1.0, 0.0);
            EmitRectangle(lightPos, lights[i].direction, 20.0, 15.0);
        } else if (lights[i].type == 2) {
            geomColor = vec3(0.0, 0.0, 1.0);
            float coneLength = 50.0;
            EmitCone(lightPos, lights[i].direction, lights[i].cutOff, lights[i].outerCutOff, coneLength);
        } else if (lights[i].type == 3) {
            geomColor = vec3(1.0, 1.0, 0.0);
            EmitRectangle(lightPos, lights[i].direction, 20.0, 15.0);
        }
    }
}

