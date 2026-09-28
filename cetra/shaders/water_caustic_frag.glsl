#version 330 core

/*
 * The lattice's concentration, interpolated across each landed cell and summed where cells
 * overlap -- a fold lands two sheets of light on one spot, and the additive blend is what adds
 * them.
 *
 * Written to every channel; the blend colour the pass sets is the colour of the band of the
 * spectrum this trace was for, white for the one trace the surface spreads itself (spec 13.4).
 */

in float gIntensity;
out vec4 Caustic;

void main() {
    Caustic = vec4(gIntensity);
}
