#version 330 core

/*
 * Diffraction glare (spec 13.4), last pass: the two transformed-back grids unpacked into one RGB
 * image of the frame's corner of the grid, and the source's scale undone. Its viewport is that
 * corner, so the tonemap reads it across the whole frame.
 */

in vec2 TexCoords;
out vec4 Glare;

uniform sampler2D glareRedGreen;
uniform sampler2D glareBlue;

void main() {
    ivec2 id = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(glareRedGreen, id, 0);
    vec4 b = texelFetch(glareBlue, id, 0);
    Glare = vec4(max(vec3(a.x, a.z, b.x), 0.0) * 1.0e3, 1.0);
}
