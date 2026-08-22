#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Specular probe projection (spec 11.70): resample one roughness level of a
// probe's prefiltered cube into one octahedral row of the atlas.
//
// There is deliberately no GGX importance sampling here. ibl_prefilter_cubemap
// already ran the tested kernel into a mip chain, and row r reads mip r, so the
// BRDF has exactly one implementation in the engine and this pass only changes
// the projection the result is stored in. Writing a second importance sampler
// against an octahedral target would have been a second answer to a question
// already answered, differing in exactly the ways nobody would notice.
//
// Covers the whole BORDERED tile, gutter included, for the reason gi_project
// states: a gutter texel's value is a pure function of a direction, so
// computing it is the same work as copying the interior texel that holds it,
// minus a second pass that would have to read the atlas while the atlas is the
// render target -- undefined in GL 4.1, which has no texture barrier.

uniform samplerCube sourceCube;
uniform vec2 tileOrigin; // atlas texel of this row's BORDER, lower-left
uniform float tileRes;   // interior edge length in texels
uniform float sourceLod; // prefiltered mip this row carries

#include "octahedral.glsl"

void main() {
    // Position within the bordered tile: 0 and tileRes+1 are the gutter ring.
    vec2 local = octWrapToInterior(floor(gl_FragCoord.xy - tileOrigin), tileRes);
    // ...then to the interior's own 0-based coordinates, which is what the
    // octahedral mapping and every consumer's UV are expressed in.
    vec3 N = octDirFromTexel(local - 1.0, tileRes);

    FragColor = vec4(textureLod(sourceCube, N, sourceLod).rgb, 1.0);
}
