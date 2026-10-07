#version 330 core

// The CRT's signal (spec 13.28): the finished picture, display-encoded at the window's size,
// brought down to the few hundred lines a console sent a television, in linear light. Lottes'
// CRTS draws from a picture already that small and says so ("make sure input to this filter is
// already low-resolution"), so the window-sized picture is area-filtered here first.

in vec2 TexCoords;
out vec4 FragColor;

#include "display.glsl"

uniform sampler2D pictureTex; // the finished picture, display-encoded, window-sized, bilinear
uniform vec2 footprint;       // one signal texel's extent in pictureTex's uv

// 4x4 bilinear taps spread across the texel's footprint, which covers a footprint up to about
// eight pixels across, 240 lines on an 1800-line window. Each tap is decoded before the taps are
// averaged, so the average is of light -- but for the bilinear blend within a tap, which is of
// codes, across two pixels.
void main()
{
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 at = TexCoords + (vec2(float(x), float(y)) - 1.5) * 0.25 * footprint;
            sum += displayDecode(texture(pictureTex, at).rgb);
        }
    }
    FragColor = vec4(sum * (1.0 / 16.0), 1.0);
}
