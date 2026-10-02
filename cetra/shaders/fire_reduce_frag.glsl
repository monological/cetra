#version 330 core

// What a GRID fire casts (spec 13.14), summed in two passes into two texels the CPU reads back.
//
// mode 0, one fragment per slice of the grid: each slice's sums over its cells, as two targets:
//   (intensity cd, and its first moments x, y, z) and (emitted r, g, b as adapted, heat W).
// The intensity is the volume integral of the emission's luminance, which treats the flame as
// optically thin: a flame absorbs little of its own light, and a thick one is over-counted. The
// heat is what the gas sheds by the cooling law, which in balance is what the burning adds.
// mode 1: the slices' sums summed, two texels per fire, the first moments in x and the colour in
// y. The order of every sum is fixed, so the answer is the same every run.

#include "fire_constants.glsl"
#include "fire_grid.glsl"
#include "blackbody.glsl"

uniform int mode;
uniform sampler2D scalarTex; // mode 0
uniform sampler2D partial0;  // mode 1
uniform sampler2D partial1;
uniform ivec4 tiles;         // mode 1: the partials' size in xy
uniform int resultBase;      // mode 1: the first of this fire's two texels
uniform vec3 boxMin;
uniform float cell;
uniform float ambient;
uniform float peakRise;
uniform float cooling;
uniform float sootAbsorption;
uniform float blueCore;
uniform vec3 blueColor; // fire_blue_color: unclamped, luminance 1
uniform float brightness;
uniform mat3 adaptation;

layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;

void main() {
    if (mode == 1) {
        int which = int(gl_FragCoord.x) - resultBase;
        vec4 sum = vec4(0.0);
        for (int y = 0; y < tiles.y; y++)
            for (int x = 0; x < tiles.x; x++)
                sum += which == 0 ? texelFetch(partial0, ivec2(x, y), 0)
                                  : texelFetch(partial1, ivec2(x, y), 0);
        out0 = sum;
        out1 = vec4(0.0);
        return;
    }
    ivec2 tile = ivec2(gl_FragCoord.xy);
    int z = tile.y * tilesX + tile.x;
    out0 = vec4(0.0);
    out1 = vec4(0.0);
    if (z >= gridSize.z)
        return;
    float volume = cell * cell * cell;
    float a = cooling / max(peakRise * peakRise * peakRise * peakRise, 1.0);
    vec3 rgbSum = vec3(0.0);
    for (int y = 0; y < gridSize.y; y++) {
        for (int x = 0; x < gridSize.x; x++) {
            ivec3 c = ivec3(x, y, z);
            vec4 s = texelFetch(scalarTex, fireAtlasTexel(c), 0);
            float theta = max(s.x, 0.0);
            float lum;
            vec3 rgb = blackbodyNits(ambient + theta, lum);
            float sigma = max(s.z, 0.0) * sootAbsorption;
            float glow = blueCore * max(s.w, 0.0);
            float e = (sigma * lum + glow) * volume * brightness;
            vec3 w = boxMin + (vec3(c) + 0.5) * cell;
            out0 += vec4(e, e * w);
            rgbSum += (sigma * rgb + glow * blueColor) * volume * brightness;
            out1.w += FIRE_AIR_RHO_CP * a * theta * theta * theta * theta * volume;
        }
    }
    // Adapted, then clamped into the gamut: the order the march draws in.
    out1.rgb = max(adaptation * rgbSum, vec3(0.0));
}
