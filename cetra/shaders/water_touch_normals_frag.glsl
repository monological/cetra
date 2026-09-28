#version 330 core

/*
 * Touch ripples' gradient (spec 13.4): the height and its two slopes, by central differences,
 * so the surface reads one filtered texel where it would take five. .r height, .g dh/dx, .b
 * dh/dz, all in world units.
 */

in vec2 TexCoords;
out vec4 Slopes;

uniform sampler2D touchField;
uniform float touchTexel; // world units per texel

void main() {
    vec2 px = 1.0 / vec2(textureSize(touchField, 0));
    float hx = texture(touchField, TexCoords + vec2(px.x, 0.0)).r -
               texture(touchField, TexCoords - vec2(px.x, 0.0)).r;
    float hz = texture(touchField, TexCoords + vec2(0.0, px.y)).r -
               texture(touchField, TexCoords - vec2(0.0, px.y)).r;
    Slopes = vec4(texture(touchField, TexCoords).r, hx / (2.0 * touchTexel),
                  hz / (2.0 * touchTexel), 0.0);
}
