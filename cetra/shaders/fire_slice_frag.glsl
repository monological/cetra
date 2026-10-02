#version 330 core

// --fire-slice (spec 13.14): one z slice of a GRID fire's fields, drawn flat into a corner of the
// frame -- the only way to see the simulation apart from how it is lit and drawn.
//   0 temperature above ambient, black to white over 0..1500 K
//   1 soot, 0..2 ppm      2 fuel, 0..1      3 speed, 0..4 m/s      4 reaction, 0..20 per second

in vec2 TexCoords;

#include "fire_grid.glsl"

uniform sampler2D scalarTex;
uniform sampler2D velocityTex;
uniform int field;
uniform int slice;

out vec4 FragColor;

// Black through red and yellow to white: a heat ramp.
vec3 ramp(float x) {
    x = clamp(x, 0.0, 1.0);
    return clamp(vec3(3.0 * x, 3.0 * x - 1.0, 3.0 * x - 2.0), 0.0, 1.0);
}

void main() {
    ivec3 c = ivec3(int(TexCoords.x * float(gridSize.x)), int(TexCoords.y * float(gridSize.y)),
                    clamp(slice, 0, gridSize.z - 1));
    c = min(c, gridSize - 1);
    vec4 s = texelFetch(scalarTex, fireAtlasTexel(c), 0);
    float v;
    if (field == 0)
        v = s.x / 1500.0;
    else if (field == 1)
        v = s.z / 2.0;
    else if (field == 2)
        v = s.y;
    else if (field == 3)
        v = length(texelFetch(velocityTex, fireAtlasTexel(c), 0).xyz) / 4.0;
    else
        v = s.w / 20.0;
    FragColor = vec4(ramp(v), 1.0);
}
