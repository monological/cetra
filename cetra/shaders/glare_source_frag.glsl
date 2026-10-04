#version 330 core

/*
 * Diffraction glare (spec 13.4, after Clearwater), first pass: the light bright enough to star.
 *
 * The frame, shrunk into the corner of the transform's grid, keeping only what exceeds the
 * threshold -- the sun and its glints, which is what a lens's aperture diffracts visibly. Scaled
 * down by `glareSourceScale` on the way in, and back up on the way out, so a glint of thousands
 * keeps its fraction bits through the transform's many additions.
 *
 * Every frame texel the grid texel covers is thresholded ON ITS OWN and then averaged, never
 * averaged and then thresholded: the tonemap takes each texel's light past the threshold out of
 * the frame (spec 13.5), and this must carry exactly that. A glint is often one bright texel
 * among dim ones, whose block average never reaches the threshold -- thresholding the average
 * put back 58% of what the tonemap took, and every glint came out darker than without the glare.
 *
 * Packed as the transform wants it: red and green as the real and imaginary parts of ONE complex
 * signal in .xy, blue alone in .zw. Both inputs are real, so the multiply can separate red's
 * spectrum from green's again, and one transform carries all three channels.
 */

in vec2 TexCoords;
out vec4 Packed;

uniform sampler2D hdrTex;
uniform float glareThreshold;
uniform float glareSourceScale;
uniform vec2 glareFill; // the grid texels the frame is shrunk into

#include "glare_threshold.glsl"
#include "view.glsl"

// The most frame texels a grid texel may cover along an axis; a 4K frame shrunk into the grid
// covers about ten.
const int GLARE_BLOCK_MAX = 16;

void main() {
    ivec2 size = textureSize(hdrTex, 0);
    vec2 scale = vec2(size) / glareFill;
    ivec2 lo = ivec2(floor((gl_FragCoord.xy - 0.5) * scale));
    ivec2 hi = min(ivec2(floor((gl_FragCoord.xy + 0.5) * scale)), size);
    vec3 c = vec3(0.0);
    int count = 0;
    for (int y = 0; y < GLARE_BLOCK_MAX; y++) {
        if (lo.y + y >= hi.y)
            break;
        for (int x = 0; x < GLARE_BLOCK_MAX; x++) {
            if (lo.x + x >= hi.x)
                break;
            c += glareAboveThreshold(sceneLight(texelFetch(hdrTex, lo + ivec2(x, y), 0).rgb),
                                     glareThreshold);
            count++;
        }
    }
    c /= float(max(count, 1));
    Packed = vec4(min(c, vec3(80000.0)) * glareSourceScale, 0.0);
}
