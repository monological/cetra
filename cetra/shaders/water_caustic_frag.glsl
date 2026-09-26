#version 330 core

/*
 * The lattice's concentration, interpolated across each landed cell and summed where cells
 * overlap -- a fold lands two sheets of light on one spot, and the additive blend is what adds
 * them.
 */

in float gIntensity;
out float Caustic;

void main() {
    Caustic = gIntensity;
}
