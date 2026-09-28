#version 330 core

/*
 * Diffraction glare (spec 13.4, after Clearwater), first pass: the light bright enough to star.
 *
 * The frame, shrunk into the corner of the transform's grid, keeping only what exceeds the
 * threshold -- the sun and its glints, which is what a lens's aperture diffracts visibly. Four
 * bilinear taps cover the downscale. Scaled down by GLARE_SOURCE_SCALE on the way in and back up
 * on the way out, so a glint of thousands keeps its fraction bits through the transform's many
 * additions.
 *
 * Packed as the transform wants it: `glareChannels` 0 puts red and green in as two complex
 * signals, real parts only, 1 puts blue in alone.
 */

in vec2 TexCoords;
out vec4 Packed;

uniform sampler2D hdrTex;
uniform float glareThreshold;
uniform int glareChannels;

const float GLARE_SOURCE_SCALE = 1.0e-3;

void main() {
    vec2 px = 1.0 / vec2(textureSize(hdrTex, 0));
    // The grid's texel covers four or five of the frame's; four bilinear taps a texel either
    // side of centre average sixteen of them.
    vec3 c = vec3(0.0);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            c += texture(hdrTex, TexCoords + (vec2(x, y) - 0.5) * px * 2.0).rgb;
    c *= 0.25;
    float l = max(max(c.r, c.g), c.b);
    c *= max(l - glareThreshold, 0.0) / max(l, 1.0e-4);
    c = min(c, vec3(80000.0)) * GLARE_SOURCE_SCALE;
    Packed = glareChannels == 0 ? vec4(c.r, 0.0, c.g, 0.0) : vec4(c.b, 0.0, 0.0, 0.0);
}
