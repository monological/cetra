#version 330 core

// What a GRID fire casts (spec 13.14), the first of the passes that sum it into the two texels
// the CPU reads back: one fragment per row of cells (y, z), its sums across x, as
//   (intensity cd, and its first moments x, y, z) and (emitted r, g, b, heat W).
// fire_sum_frag then sums each slice's rows, and the slices. Every sum runs in a fixed order, so
// the answer is the same every run.
//
// The intensity is the luminance of what the march draws, summed over the volume. That treats
// the flame as optically thin: a flame absorbs little of its own light, so a thick one is
// over-counted. The heat is what the gas sheds by the cooling law, which in balance is what the
// burning adds.

#include "fire_emission.glsl"

uniform sampler2D scalarTex;
uniform vec3 boxMin; // metres, local to the fire's origin
uniform float cell;
uniform float coolingRate; // fire_cooling_rate

layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;

void main() {
    int y = int(gl_FragCoord.x);
    int z = int(gl_FragCoord.y);
    float volume = cell * cell * cell;
    out0 = vec4(0.0);
    out1 = vec4(0.0);
    for (int x = 0; x < gridSize.x; x++) {
        ivec3 c = ivec3(x, y, z);
        vec3 p = vec3(c) + 0.5;
        FireGas g = fireGas(texelFetch(scalarTex, fireAtlasTexel(c), 0));
        float absorption;
        vec3 rgb = fireEmission(fireFadeAtEdge(g, p), absorption) * volume;
        float e = fireLuminance(rgb);
        out0 += vec4(e, e * (boxMin + p * cell));
        float theta = max(g.rise, 0.0);
        out1 += vec4(rgb, FIRE_AIR_RHO_CP * coolingRate * theta * theta * theta * theta * volume);
    }
}
