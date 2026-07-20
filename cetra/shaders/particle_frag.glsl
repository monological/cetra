#version 330 core

// M1 particle shading: an unlit, HDR-bright soft sprite with a life alpha
// envelope. Lighting + CSM shadow (M3) and soft-particle depth fade (M4) layer
// in later. Output is premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
// See specs/5.0-particle-system.md.

in vec2 vCorner;
in vec4 vColor;
in float vLifeFrac;

layout(location = 0) out vec4 FragColor;

uniform float hdrGain; // push color above 1.0 so bloom catches the motes

void main() {
    // Soft round sprite from the radial distance across the quad.
    float r = length(vCorner);
    float a = smoothstep(1.0, 0.35, r) * vColor.a;

    // Life alpha envelope: fade in over the first 10%, out over the last 30%.
    float fadeIn = smoothstep(0.0, 0.1, vLifeFrac);
    float fadeOut = 1.0 - smoothstep(0.7, 1.0, vLifeFrac);
    a *= fadeIn * fadeOut;

    vec3 rgb = vColor.rgb * hdrGain;

    FragColor = vec4(rgb * a, a);
}
