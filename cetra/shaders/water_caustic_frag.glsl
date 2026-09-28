#version 330 core

/*
 * The lattice's concentration, interpolated across each landed cell and summed where cells
 * overlap -- a fold lands two sheets of light on one spot, and the additive blend is what adds
 * them.
 *
 * Two layouts (spec 13.4). Traced once, .r is the concentration and .gb the concentration
 * times how its landing moves per unit index -- summed by the blend and averaged by the mips,
 * so .gb / .r is the local dispersion weighted by the light that carries it, which the
 * surface's lookup spreads the spectrum along. Traced per band, every channel carries the
 * concentration and the blend colour is the band's.
 */

in float gIntensity;
in vec2 gPerIndex;
out vec4 Caustic;

uniform int causticPerIndex; // 1 = write the dispersion beside the concentration

void main() {
    Caustic = causticPerIndex == 1 ? vec4(gIntensity, gIntensity * gPerIndex, 0.0)
                                   : vec4(gIntensity);
}
