#version 330 core

/*
 * Diffraction glare (spec 13.4), last pass: the transformed-back grid unpacked into one RGB image
 * of the frame's corner of the grid -- red and green are the real and imaginary parts of .xy,
 * blue the real part of .zw -- and the source's scale undone. Its viewport is that corner, so the
 * tonemap reads it across the whole frame.
 */

in vec2 TexCoords;
out vec4 Glare;

uniform sampler2D glareResult;
uniform float glareSourceScale;

void main() {
    vec4 a = texelFetch(glareResult, ivec2(gl_FragCoord.xy), 0);
    Glare = vec4(max(vec3(a.x, a.y, a.z), 0.0) / glareSourceScale, 1.0);
}
