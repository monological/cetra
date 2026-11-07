#version 330 core

// A canvas view into the frame it is drawn over (spec 14.0): the view's own target, copied into
// the view's rectangle, opaque. The viewport is that rectangle, so this runs only inside it. While
// the view dims (spec 14.24), each tagged pixel fades toward the view's colour, less near a light.

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uView;
uniform ivec4 uOrigin;  // xy: the rectangle's lower left corner in the frame, framebuffer pixels
uniform sampler2D uLit; // r a lit shape's coverage, g any tagged shape's, mipmapped
uniform float uDim;     // how far a tagged pixel away from a light fades; 0 = none
uniform vec3 uDimTo;    // toward this colour
uniform float uRoom;    // framebuffer pixels round a light the fading eases off over

// How lit a pixel is: on a light 1; within uRoom of one, easing to 0 at uRoom, by the largest of
// rings of taps round it, each read at a level of detail as wide as its spacing so a thin light
// between taps still registers.
float lightAt(ivec2 p) {
    float lit = texelFetch(uLit, p, 0).r;
    if (uRoom < 1.0)
        return lit;
    vec2 size = vec2(textureSize(uLit, 0));
    vec2 uv = (vec2(p) + 0.5) / size;
    const int RINGS = 4, SPOKES = 12;
    for (int r = 1; r <= RINGS; r++) {
        float radius = uRoom * float(r) / float(RINGS);
        float spacing = 6.2831853 * radius / float(SPOKES);
        float lod = log2(max(spacing, 1.0));
        float falloff = 1.0 - smoothstep(0.0, uRoom, radius - uRoom / float(RINGS));
        for (int s = 0; s < SPOKES; s++) {
            float a = 6.2831853 * (float(s) + 0.5 * float(r & 1)) / float(SPOKES);
            vec2 at = uv + radius * vec2(cos(a), sin(a)) / size;
            float v = clamp(textureLod(uLit, at, lod).r * 4.0, 0.0, 1.0);
            lit = max(lit, v * falloff);
        }
    }
    return lit;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - uOrigin.xy;
    vec3 colour = texelFetch(uView, p, 0).rgb;
    if (uDim > 0.0) {
        float tagged = texelFetch(uLit, p, 0).g;
        colour = mix(colour, uDimTo, uDim * tagged * (1.0 - lightAt(p)));
    }
    FragColor = vec4(colour, 1.0);
}
