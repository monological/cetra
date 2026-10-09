#version 330 core

// The town map's ink (spec 13.43): the UI's own vertex layout (ui_vert.glsl), passing on what
// map_ink_frag reads.
layout(location = 0) in vec2 aPos; // window points, top-left origin, +Y down
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aRect;
layout(location = 4) in vec4 aParams;
layout(location = 5) in vec4 aBorder;

out vec2 vUV;
out vec4 vColor;

uniform mat4 uProjection;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
