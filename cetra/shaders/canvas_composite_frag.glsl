#version 330 core

// A canvas view into the frame it is drawn over (spec 14.0): the view's own target, copied into
// the view's rectangle, opaque. The viewport is that rectangle, so this runs only inside it.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uView;
uniform ivec4 uOrigin; // xy: the rectangle's lower left corner in the frame, framebuffer pixels

void main()
{
    FragColor = vec4(texelFetch(uView, ivec2(gl_FragCoord.xy) - uOrigin.xy, 0).rgb, 1.0);
}
