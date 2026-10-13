#version 330 core

// The haunting's picture (spec 13.52), after the tone map: every hue drained out of the frame and
// its lightness put back as one colour, `hauntGrade.rgb`, by `hauntGrade.w` of the way. Display
// values, so the colour is how the red reads on screen.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D sceneColor;
uniform vec4 hauntGrade;

void main()
{
    vec3 color = texelFetch(sceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    float lightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    FragColor = vec4(mix(color, lightness * hauntGrade.rgb, hauntGrade.w), 1.0);
}
