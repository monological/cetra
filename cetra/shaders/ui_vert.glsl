#version 330 core

// The UI's own VAO, so these locations are a private ledger -- common.h's
// GL_ATTR_* numbering governs MESH VAOs and nothing here collides with it.
layout(location = 0) in vec2 aPos;    // window points, top-left origin, +Y down
layout(location = 1) in vec2 aUV;     // atlas UV for glyphs, 0..1 for textures
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aRect;   // the quad's min.xy / max.xy, for the corner SDF
layout(location = 4) in vec4 aParams; // x corner radius, y border width, z mode, w unused
layout(location = 5) in vec4 aBorder;

out vec2 vUV;
out vec4 vColor;
out vec2 vPos;
out vec4 vRect;
out vec4 vParams;
out vec4 vBorder;

// Points, not framebuffer pixels: the UI is authored in the same space the text
// renderer's ortho and GLFW's cursor position already use, so nothing converts.
uniform mat4 uProjection;

void main() {
    vUV = aUV;
    vColor = aColor;
    vPos = aPos;
    vRect = aRect;
    vParams = aParams;
    vBorder = aBorder;
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
}
