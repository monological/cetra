#version 330 core

// A post pass that paints a flat rectangle over the frame (spec 13.29's fixture): `markRect` is
// x0, y0, x1, y1 in the frame's 0..1, `markColor` what goes inside it. Everything outside is the
// frame, copied texel for texel, so a pass whose rectangle is empty is the identity.

in vec2 TexCoords;
out vec4 FragColor;

// Unused here; included so the runtime resolver is what this shader compiles through.
#include "color.glsl"

uniform sampler2D sceneColor;
uniform vec4 markRect;
uniform vec4 markColor;

void main()
{
    vec3 color = texelFetch(sceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    bool inside = all(greaterThanEqual(TexCoords, markRect.xy)) && all(lessThan(TexCoords, markRect.zw));
    FragColor = vec4(inside ? markColor.rgb : color, 1.0);
}
