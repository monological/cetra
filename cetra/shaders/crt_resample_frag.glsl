#version 330 core

// The CRT's signal (spec 13.28): the finished picture, display-encoded at the window's size,
// brought down to the few hundred lines a console sent a television, in linear light. Lottes'
// CRTS draws from a picture already that small and says so ("make sure input to this filter is
// already low-resolution"), so the window-sized frame is area-filtered here first: each texel of
// the signal averages the window pixels it covers, decoded with the engine's own display encode
// so the average is of light and not of codes.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D pictureTex; // the finished frame, display-encoded, window-sized, bilinear
uniform vec2 footprint;       // one signal texel's extent in pictureTex's uv

// 4x4 bilinear taps spread across the texel's footprint: each averages the 2x2 pixels round it,
// which covers a footprint up to about eight pixels across, 240 lines on a 1800-line window.
void main()
{
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 at = TexCoords + (vec2(float(x), float(y)) - 1.5) * 0.25 * footprint;
            sum += pow(clamp(texture(pictureTex, at).rgb, 0.0, 1.0), vec3(2.2));
        }
    }
    FragColor = vec4(sum * (1.0 / 16.0), 1.0);
}
