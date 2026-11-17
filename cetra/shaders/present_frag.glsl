#version 330 core

// A finished, display-encoded picture copied 1:1 into the bound framebuffer at `pictureOrigin`:
// a view's picture into its rectangle, as it is, or the window's picture into the window with
// the output dither, which nothing before it applied because it is the 8-bit write.

in vec2 TexCoords;
out vec4 FragColor;

#include "dither.glsl"

uniform sampler2D pictureTex;
uniform vec2 pictureOrigin; // the picture's lower-left pixel in the framebuffer
uniform int ditherEnabled;
uniform float ditherStrength;

void main()
{
    vec3 color = texelFetch(pictureTex, ivec2(gl_FragCoord.xy - pictureOrigin), 0).rgb;
    if (ditherEnabled == 1)
        color = applyDither(color, gl_FragCoord.xy, ditherStrength);
    FragColor = vec4(color, 1.0);
}
