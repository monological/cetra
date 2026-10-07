#version 330 core

// The finished picture into the window (spec 13.29), when something drew after the tone map and
// no CRT is on to show it: the picture is display-encoded and window-sized, so this is a copy
// with the output dither, which the tone map left out because it was not the 8-bit write.

in vec2 TexCoords;
out vec4 FragColor;

#include "dither.glsl"

uniform sampler2D pictureTex;
uniform int ditherEnabled;
uniform float ditherStrength;

void main()
{
    vec3 color = texelFetch(pictureTex, ivec2(gl_FragCoord.xy), 0).rgb;
    if (ditherEnabled == 1)
        color = applyDither(color, gl_FragCoord.xy, ditherStrength);
    FragColor = vec4(color, 1.0);
}
