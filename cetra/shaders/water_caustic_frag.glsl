#version 330 core

/*
 * The lattice's concentration, interpolated across each landed cell and summed where cells
 * overlap -- a fold lands two sheets of light on one spot, and the additive blend is what adds
 * them.
 *
 * Written to every channel; the colour mask the pass sets keeps the one this trace was for
 * (spec 13.4).
 */

in float gIntensity;
out vec4 Caustic;

void main() {
    Caustic = vec4(gIntensity);
}
