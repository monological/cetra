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
 * ADVECTED, since spec 11.44, by a backtrace: this texel's history is read from where the
 * water that is here now WAS a frame ago, not from this texel. Two currents carry it and
 * they are different things.
 *
 * The ORBITAL part is already handled and costs nothing here: the surface reads foam at the
 * parcel's undisplaced label (water_frag's SurfParam), so foam rides a wave's own back-and-
 * forth motion by construction. What that cannot express is DRIFT -- the mean transport a
 * wind sea has along the wind, which a linear wave field has no term for at all and which is
 * what makes real whitewater stream downwind instead of sitting where it was born.
 *
 * So the backtrace here is the drift alone, and it is per BAND: the two channels live in
 * two tiling spaces, so the same world-space drift is a different UV step in each and one
 * offset would smear the two. Two fetches rather than one, on a 128 square pass.
 *
 * ONE target, two bands in two channels -- the long and medium bands, the only two that
 * still vote on foam selection (spec 11.47 dropped the short band's, see water_frag). Each
 * channel is in its OWN band's tiling space, since that is the space its Jacobian was read
 * in -- so the consumer samples this texture twice at two UVs rather than once. The blue
 * channel is written zero and read by nobody; kept rather than reshaping the target because
 * a third band could return to it without a format change, and RGBA costs the same as RGB
 * on every hardware this targets.
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
// How fast foam streams downwind, in METRES per second, converted here by the same
// waterUnitsPerMetre every other physical length in this water goes through. A few per cent
// of the wind speed is what the literature gives for surface drift; this is a flat rate
// rather than a fraction of it because the sea state is not a uniform here.
uniform float foamDriftSpeed;

// For oceanBandJacobian and cascadeChoppiness. The Jacobian has to be the SAME expression
// the surface shades from, or foam is selected off a different fold than the one drawn.
#include "ocean.glsl"

// Floor on the divisor. A deeply folded texel has a Jacobian at or below zero, and its
// recovery would run backwards or divide by nothing.
const float FOAM_MIN_J = 0.5;

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);

    /*
     * Backtrace: where the water under this texel drifted FROM. Upwind by one frame's
     * travel, expressed in each band's own tiling space, and wrapped -- the field is
     * periodic, so a step off one edge is a step onto the other and needs no clamp.
     *
     * LINEAR sampling for the history, where the Jacobians below are texelFetch: a
     * backtrace lands between texels by definition, and snapping it to the nearest one
     * would quantise the drift into stair-steps at exactly the rate it advances.
     */
    vec2 prev = vec2(1.0);
    if (foamHistoryAvailable == 1) {
        vec2 uv = (vec2(coord) + 0.5) / vec2(textureSize(prevFoam, 0));
        vec2 travel = waterWindDir * (foamDriftSpeed * waterUnitsPerMetre * foamDt);
        prev = vec2(texture(prevFoam, fract(uv - travel / cascadeLength[0])).r,
                    texture(prevFoam, fract(uv - travel / cascadeLength[1])).g);
    }

    vec4 field0Long = oceanCascadeTexel(0, 0, coord);
    vec4 field0Med = oceanCascadeTexel(1, 0, coord);

    vec2 j;
    j.x = oceanBandJacobian(field0Long, oceanCascadeTexel(0, 1, coord), cascadeChoppiness[0]);
    j.y = oceanBandJacobian(field0Med, oceanCascadeTexel(1, 1, coord), cascadeChoppiness[1]);

    vec2 recovered = prev + foamDt * foamDecay / max(j, vec2(FOAM_MIN_J));

    /*
     * THE BIRTH GATE (oceanCrestGate, ocean.glsl; spec 11.47): a fold only snaps the record
     * down when its OWN band's elevation is near that band's own crest. `.b` of the
     * target-0 texel this pass already fetched for the Jacobian is that band's height in
     * metres (ocean.glsl:611-628) -- no extra sample. Per band and not assembled the way
     * water_frag's is: each channel lives in its own tiling space, so one texel index is a
     * different world position in each and there is no single elevation to ask about here.
     */
    vec2 crestGate = vec2(oceanCrestGate(field0Long.b, cascadeHeightVar[0]),
                          oceanCrestGate(field0Med.b, cascadeHeightVar[1]));
    // Gated only here, at 1.0 (open water, no fold on record) where the gate is shut. The
    // divisor above stays on the raw Jacobian regardless -- see the file comment on `recovered`.
    vec2 jSnap = mix(vec2(1.0), j, crestGate);
    // The minimum is what makes this a snap rather than a blend: a fold takes effect on the
    // frame it happens, where recovery is always gradual.
    Foam = vec4(min(jSnap, recovered), 0.0, 1.0);
}
