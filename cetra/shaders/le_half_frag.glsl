#version 330 core

/*
 * Local exposure (spec 13.19), first pass: the frame at half resolution, what the bilateral grid
 * is built from (Unreal builds it from half-res texels too). The mean colour of each 2x2 block in
 * .rgb, for the blurred luminance, and the log2 luminance of that mean in .a, for the grid. In
 * EXPOSED space -- the buffer arrives pre-exposed -- so the grid follows the camera and a scene a
 * thousand times brighter under an exposure a thousand times smaller builds the same grid.
 */

in vec2 TexCoords;
out vec4 Half;

uniform sampler2D hdrTex;

#include "view.glsl"

void main() {
    ivec2 size = textureSize(hdrTex, 0);
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    vec3 c = vec3(0.0);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            c += sceneLight(texelFetch(hdrTex, min(p + ivec2(x, y), size - 1), 0).rgb);
    c *= 0.25;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    Half = vec4(c, log2(max(lum, 1.0e-8)));
}
