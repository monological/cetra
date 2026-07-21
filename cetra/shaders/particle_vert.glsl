#version 330 core

// Static unit quad (slot 0) expanded into a camera-facing billboard per
// instance. Per-instance attributes live on free slots >=9 (see common.h /
// ParticleInstanceData). See specs/5.0-particle-system.md.
layout(location = 0) in vec2 aCorner; // unit quad corner in [-1,1]
layout(location = 9) in vec3 iCenter;
layout(location = 10) in vec4 iParams; // x=size, y=rotation, z=lifeFrac, w=seed
layout(location = 11) in vec4 iColor;

uniform mat4 view;
uniform mat4 projection;

out vec2 vCorner;
out vec4 vColor;
out float vLifeFrac;
out vec3 vWorldPos;
out float vViewZ;

void main() {
    float size = iParams.x;
    float rot = iParams.y;

    // Rows of the view matrix are the camera's world-space right / up axes.
    vec3 camRight = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 camUp = vec3(view[0][1], view[1][1], view[2][1]);

    // Roll the corner around the sprite center.
    float c = cos(rot);
    float s = sin(rot);
    vec2 rc = vec2(aCorner.x * c - aCorner.y * s, aCorner.x * s + aCorner.y * c);

    vec3 worldPos = iCenter + (camRight * rc.x + camUp * rc.y) * size;
    vec4 viewPos = view * vec4(worldPos, 1.0);
    gl_Position = projection * viewPos;

    vCorner = aCorner;
    vColor = iColor;
    vLifeFrac = iParams.z;
    vWorldPos = worldPos;
    vViewZ = viewPos.z; // negative (looking down -Z)
}
