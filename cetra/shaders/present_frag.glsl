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
// A view's outlines (spec 14.39), premultiplied and of the picture's size, laid over it here so the
// picture -- a still view's average -- is never drawn into.
uniform sampler2D overlayTex;
uniform int overlayOn;

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy - pictureOrigin);
    vec3 color = texelFetch(pictureTex, at, 0).rgb;
    if (overlayOn == 1) {
        vec4 over = texelFetch(overlayTex, at, 0);
        color = color * (1.0 - over.a) + over.rgb;
    }
    if (ditherEnabled == 1)
        color = applyDither(color, gl_FragCoord.xy, ditherStrength);
    FragColor = vec4(color, 1.0);
}
