#version 330 core

/*
 * Foam accumulation (spec 11.42).
 *
 * Whitewater is not a property of the surface NOW. A crest folds, air is entrained, and the
 * foam it leaves sits on the water long after the wave that made it has gone -- so foam
 * selected from this frame's Jacobian alone appears and vanishes with the crest, which
 * reads as a moving white stripe rather than as something the sea left behind.
 *
 * This carries one value per texel per band that SNAPS DOWN the moment the horizontal map
 * folds and recovers slowly toward 1 afterwards, so it is a running minimum with a leak.
 * The recovery is divided by the Jacobian, so water that is still folding holds its foam
 * and water that has reopened gives it up.
 *
 * Per-texel with no neighbourhood, which is why it is a fragment pass and not a compute
 * kernel -- the same reason the spectrum evolution and the FFT stages are.
 *
 * NOT advected. Real foam rides the surface current, and the reference this water is
 * derived from backtraces it through a velocity field it has and this one does not. What
 * is here is birth and decay in place, which is the half that makes whitecaps linger.
 *
 * ONE target, three bands in three channels. Each channel is in its OWN band's tiling
 * space, since that is the space its Jacobian was read in -- so the consumer samples this
 * texture three times at three UVs rather than once, and three separate textures would
 * have cost three sampler declarations in a program that has one to spare.
 */

layout(location = 0) out vec4 Foam;

uniform sampler2D prevFoam;
// 0 on the first frame, where there is no history and the sea starts un-foamed. Seeding
// from zero instead would put the whole ocean under whitewater for the first second.
uniform int foamHistoryAvailable;
uniform float foamDt;
// Recovery rate toward unfolded. Lower lingers longer; this is the knob that decides
// whether a crest leaves a streak or a smear.
uniform float foamDecay;

// For oceanBandJacobian and cascadeChoppiness. The Jacobian has to be the SAME expression
// the surface shades from, or foam is selected off a different fold than the one drawn.
#include "ocean.glsl"

// Floor on the divisor. A deeply folded texel has a Jacobian at or below zero, and its
// recovery would run backwards or divide by nothing.
const float FOAM_MIN_J = 0.5;

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec3 prev = foamHistoryAvailable == 1 ? texelFetch(prevFoam, coord, 0).rgb : vec3(1.0);

    vec3 j;
    j.x = oceanBandJacobian(texelFetch(cascade0_0, coord, 0), texelFetch(cascade0_1, coord, 0),
                            cascadeChoppiness[0]);
    j.y = oceanBandJacobian(texelFetch(cascade1_0, coord, 0), texelFetch(cascade1_1, coord, 0),
                            cascadeChoppiness[1]);
    j.z = oceanBandJacobian(texelFetch(cascade2_0, coord, 0), texelFetch(cascade2_1, coord, 0),
                            cascadeChoppiness[2]);

    vec3 recovered = prev + foamDt * foamDecay / max(j, vec3(FOAM_MIN_J));
    // The minimum is what makes this a snap rather than a blend: a fold takes effect on the
    // frame it happens, where recovery is always gradual.
    Foam = vec4(min(j, recovered), 1.0);
}
