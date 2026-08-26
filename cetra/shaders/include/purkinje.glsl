/*
 * The Purkinje shift (spec 11.83): the RETINA's spectral response.
 *
 * In dim light the rods take over from the cones. Rods are monochromats with a
 * blue-shifted sensitivity peak, so colour drains, the image shifts blue, and
 * reds go near-black -- which is why moonlight "looks" blue when its spectrum is
 * sunlight's. This is what makes a dark frame read as NIGHT rather than as an
 * underexposed day photo.
 *
 * WHERE THIS SITS, and it is the file's third siting rule rather than a third
 * opinion about the first two. tonemap_frag's header states that the gamma
 * encode divides stages this engine DEFINES from stages whose DATA was authored
 * in display space; that rule places this before the encode and says nothing
 * more. The rule that actually sites it is chromatic aberration's, one screen
 * further down: CA is "a LENS effect: it acts on the light before the sensor".
 * That is an OPTICAL-CHAIN ordering, and this is the sensor's own spectral
 * response -- so it lands after the lens (CA, bloom, flare) and before the tone
 * curve, which stands in for the response curve. Grain is already sited as
 * "sensor noise" after that curve; these are the two halves of one sensor,
 * correctly either side of it.
 *
 * WHAT IS CITED AND WHAT IS NOT. Kirk & O'Brien 2011 is the source for the rod
 * luminance and the mesopic blend. Their acuity and noise terms are elsewhere in
 * this file, under their own toggles. This is a look filter with a physical
 * skeleton, and calling it an implementation of the paper would be a claim the
 * code does not support.
 *
 * EVERY THRESHOLD HERE IS A LOOK CONSTANT. Literature places the photopic floor
 * near 3 cd/m^2 and full scotopic below 0.005, but this engine's sky is about
 * four decades under the real photometric scale (view.glsl) and its whole
 * day-to-night range is 4.2 stops where reality is ~17. So the ramps are
 * calibrated against this corpus, not against the eye, and purkinjeBiasEV is the
 * single knob that migrates them if the scale is ever fixed.
 */
#include "scotopic_weights.glsl"
#include "noise.glsl"

// The mesopic band, in log2 absolute scene radiance. Measured on aerial_fixture:
// daylight meters -2.47, sunset -4.37, night -6.67.
//
// GLOBAL is the whole-frame gate -- is the eye dark-ADAPTED. Its high edge sits
// 1.53 stops above daylight so the identity below is exactly zero with margin;
// its low edge sits 0.67 under the measured night.
const float PK_GLOBAL_LO = -6.0;
const float PK_GLOBAL_HI = -4.0;
// LOCAL is the per-pixel gate -- is there light HERE for the cones to answer.
// The high edge is what decides what stays coloured at night.
const float PK_LOCAL_LO = -8.0;
const float PK_LOCAL_HI = -3.0;

// How far rod pooling smears detail, in texels at full weight. Scotopic vision
// is genuinely INDISTINCT, not merely grey -- many rods share one ganglion cell,
// which buys sensitivity by spending resolution.
const float PK_ACUITY_TEXELS = 2.5;
// Rod noise amplitude at full weight, as a fraction of the local value. Photon
// arrivals are Poisson, so the visible grain rises as the signal falls; this is
// the cheap stand-in, scaled by the weight rather than by a real sqrt(N).
const float PK_NOISE_AMOUNT = 0.28;

uniform int purkinjeEnabled;
uniform float purkinjeStrength; // 0 is a bit-exact identity
uniform float purkinjeBiasEV;   // stops, added to BOTH ramps
uniform int purkinjeHasMeter;   // 0 = adaptTex holds nothing; fall back to local alone
uniform float purkinjeAcuity;   // 0 = no spatial pooling
uniform float purkinjeNoise;    // 0 = no rod noise
uniform sampler2D purkinjeAdaptTex; // the metering 1x1: mean log2 absolute cd/m^2

/*
 * The rod weight, and the two gates are MULTIPLIED because both failure cases
 * want a veto rather than a vote.
 *
 * A daylit shadow is locally dim inside a bright frame; a lamp at night is
 * locally bright inside a dim one. Blending the two drivers into one lands both
 * mid-ramp and drains both. A product lets either gate refuse outright -- and
 * gives two independent routes to an exactly-zero daylight identity.
 *
 * MEASURED, on aerial_fixture: the day frame's darkest decile sits at -7.85, so
 * without the global veto a noon shadow would take wLocal 0.997 -- essentially
 * the full shift, in sunlight. The product is a requirement, not a preference.
 *
 * Returns (w, wLocal, wGlobal); the debug view wants all three, and the caller
 * uses only .x.
 */
vec3 purkinjeWeight(vec3 c) {
    // c is finite by construction -- the caller sanitized it against
    // WS_SCENE_MAX one line up, which is what the identity below rests on.
    float lumPix = dot(c, vec3(0.2126, 0.7152, 0.0722)) * oneOverPreExposure;
    // The same 1e-8 guard lum_measure_frag uses, so both agree about black.
    float logLpix = log2(max(lumPix, 1.0e-8));
    // Edges LO < HI always: smoothstep with edge0 > edge1 is undefined in GLSL,
    // and the frame it produces is plausible rather than obviously broken.
    float wLocal = 1.0 - smoothstep(PK_LOCAL_LO + purkinjeBiasEV,
                                    PK_LOCAL_HI + purkinjeBiasEV, logLpix);

    float logLada = texelFetch(purkinjeAdaptTex, ivec2(0, 0), 0).r;
    /*
     * Two ways the global term can be untrustworthy, and both fall back to 1.0
     * -- the local gate alone -- rather than to 0.0. A flag that silently does
     * nothing is worse than a degraded picture.
     *
     * No meter: the C side never ran the draws, and an unwritten R32F target's
     * content is undefined. The shader cannot know that, so it is told.
     *
     * NaN: lum_reduce_frag writes a DELIBERATE NaN on an empty histogram, which
     * spot metering at a small radius reaches. It matters because NaN survives
     * the guard below -- `w <= 0.0` is FALSE on NaN -- so the identity's own
     * early-out is exactly what a NaN would slip past, into a NaN frame.
     */
    float wGlobal = 1.0;
    if (purkinjeHasMeter != 0 && !isnan(logLada))
        wGlobal = 1.0 - smoothstep(PK_GLOBAL_LO + purkinjeBiasEV,
                                   PK_GLOBAL_HI + purkinjeBiasEV, logLada);

    // Strength scales the WEIGHT and nothing else. Folding it into the tint
    // instead (mix(vec3(1), TINT, strength)) still desaturates fully at w = 1,
    // so strength 0 would drain colour while looking, in code, like a control.
    return vec3(purkinjeStrength * wLocal * wGlobal, wLocal, wGlobal);
}

/*
 * Apply the shift. A BLEND toward the rod image, not Kirk & O'Brien's additive
 * rod-plus-attenuated-cone sum: an additive term adds energy, which brightens a
 * night frame and fights both the WS_* ceilings and the tone curve's shoulder --
 * and mix(c, x, 0.0) is EXACTLY c, which the additive form is not.
 */
vec3 purkinjeApply(vec3 c, vec2 uv, vec2 texel, float aoFactor, vec3 bloomAdd,
                   sampler2D sceneTex, float seed) {
    if (purkinjeEnabled == 0)
        return c;
    float w = purkinjeWeight(c).x;
    // Structural identity. smoothstep is exactly 1.0 at or past its high edge,
    // so a daylight frame gives wGlobal exactly 0 and returns here untouched --
    // exactly, not nearly. Belt and braces over a mix that is already exact.
    if (w <= 0.0)
        return c;

    /*
     * ACUITY. Rods pool spatially, so the dark parts of the frame lose detail
     * rather than only colour -- which is the half that reads as "I cannot quite
     * see" instead of "someone applied a blue filter".
     *
     * Four taps of the RAW scene, composited once with the CENTRE's aoFactor and
     * bloomAdd. Reusing the centre's is the sharpen block's own precedent, and
     * here it matters more: pooling the composited neighbours would blur the AO
     * and bloom gradients too, which belong to the lens and to the geometry
     * rather than to the retina.
     *
     * The sampler is a PARAMETER, which is this codebase's convention for an
     * include that must not know its caller's unit ledger -- sky_radiance.glsl
     * and probe_specular.glsl both do it. It also means the taps skip chromatic
     * aberration, which is correct in the small: CA displaces channels by a
     * fraction of a texel and this is a low-pass over several.
     */
    if (purkinjeAcuity > 0.0) {
        float r = PK_ACUITY_TEXELS * purkinjeAcuity * w;
        vec3 sum = texture(sceneTex, uv + vec2(texel.x, 0.0) * r).rgb +
                   texture(sceneTex, uv - vec2(texel.x, 0.0) * r).rgb +
                   texture(sceneTex, uv + vec2(0.0, texel.y) * r).rgb +
                   texture(sceneTex, uv - vec2(0.0, texel.y) * r).rgb;
        vec3 pooled = min(sum * 0.25, vec3(WS_SCENE_MAX)) * aoFactor + bloomAdd;
        c = mix(c, pooled, w);
    }

    c = mix(c, dot(c, PURKINJE_SCOTOPIC_W) * PURKINJE_ROD_TINT, w);

    /*
     * ROD NOISE, and it is a different thing from the grain stage downstream.
     * Grain is SENSOR noise laid over a finished look, which is why it sits
     * after the display encode. This is RETINAL: photon arrivals are Poisson, so
     * a dim scene is genuinely noisy to the eye before any transfer curve, and
     * it scales with how far into rod vision the pixel is.
     *
     * Rides the grain stage's own frame seed rather than carrying a second one:
     * that value is already documented as deterministic across equal --frames
     * runs, and a third source of per-frame randomness is a third thing to keep
     * that way.
     *
     * Multiplicative, so it vanishes in true black rather than lifting it -- the
     * night frame is half at the radiance floor and additive noise there would
     * print static onto pixels that carry no light at all.
     */
    if (purkinjeNoise > 0.0) {
        float n = hash21(uv * 1024.0 + seed, vec2(12.9898, 78.233)) - 0.5;
        c *= 1.0 + n * PK_NOISE_AMOUNT * purkinjeNoise * w;
    }
    return c;
}
