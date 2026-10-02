#version 330 core

// One card of a FLIPBOOK fire (spec 13.14): a quad standing on its base, turned about the vertical
// to face the camera -- flames rise, so the card turns with the eye and never leans with it. Four
// vertices from gl_VertexID as a strip; no attributes.

uniform mat4 viewProj;
uniform mat4 view;
uniform vec3 cameraPos;
uniform vec3 cardBase; // bottom centre, world metres
uniform vec2 cardSize; // width, height

out vec2 vUv; // 0..1 across the card, bottom to top
out float vViewDepth;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec3 toCamera = cameraPos - cardBase;
    toCamera.y = 0.0;
    vec3 right = dot(toCamera, toCamera) > 1e-8 ? normalize(cross(vec3(0.0, 1.0, 0.0), toCamera))
                                                : vec3(1.0, 0.0, 0.0);
    vec3 P = cardBase + right * (corner.x - 0.5) * cardSize.x + vec3(0.0, corner.y * cardSize.y, 0.0);
    vUv = corner;
    vViewDepth = -(view * vec4(P, 1.0)).z;
    gl_Position = viewProj * vec4(P, 1.0);
}
