#version 330 core

/*
 * How much light one landed cell concentrates: the beam area it came from over the area it
 * landed on. dFdx/dFdy of the source position across one target texel give the source area
 * that texel receives, so dividing by the texel's own area makes flat water exactly 1 and
 * the whole target energy-conserving -- a line is bright only because a gap beside it is dark.
 * Folded cells overlap and add under the additive blend, which is what a fold does.
 */

in vec2 vSrc; // metres
out vec4 Caustic;

uniform float causticTexelArea; // square metres per target texel

// A true focus is a singularity; this is the ceiling on one cell's contribution, and the only
// place the pass loses energy.
const float WATER_CAUSTIC_MAX = 40.0;

void main() {
    vec2 a = dFdx(vSrc);
    vec2 b = dFdy(vSrc);
    float intensity = min(abs(a.x * b.y - a.y * b.x) / causticTexelArea, WATER_CAUSTIC_MAX);
    Caustic = vec4(intensity);
}
