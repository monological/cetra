/*
 * The Purkinje shift (spec 11.83): the RETINA's spectral response.
 *
 * In dim light the rods take over from the cones. Rods are monochromats with a
 * blue-shifted sensitivity peak, so colour drains, the image shifts blue, and
 * reds go near-black -- which is why moonlight "looks" blue when its spectrum is
 * sunlight's. This is what makes a dark frame read as NIGHT rather than as an
 * underexposed day photo.
 *
 * Sited by tonemap_frag's optical-chain rule; that file's header owns the order
 * and is where a stage's position is argued.
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
// view.glsl for oneOverPreExposure and WS_SCENE_MAX. Included rather than
// inherited from the caller: include-once makes it free, and relying on
// tonemap_frag having included it four lines earlier is an ordering dependency
// between two lines of an unrelated file.
#include "view.glsl"
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
// The metering 1x1: the percentile-clipped mean log2 of absolute cd/m^2.
//
// IT IS THE METERING STATISTIC, not a neutral frame average, and that has a
// consequence worth knowing before tuning either: post.metering's mask and
// percentiles shape it, so --meter-mode, --meter-radius, --meter-low and
// --meter-high all move the rod shift -- on a frame where the exposure is
// pinned and nothing else suggests the meter is live. meter_low defaults to
// 0.70 for auto-exposure's sake, and that tuning is now load-bearing for a
// second consumer that had no say in it.
uniform sampler2D purkinjeAdaptTex;

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
/*
 * The WHOLE-FRAME gate: is the eye dark-ADAPTED. One texel and some uniforms, so
 * it is identical for every fragment and its branch is wave-coherent.
 *
 * Evaluated FIRST by purkinjeApply, and that ordering is a measurement rather
 * than taste: in daylight this is exactly 0 while the per-pixel gate below is
 * ~0.997, so testing the cheap frame-uniform term first lets a whole daylight
 * frame exit before paying a log2 and a smoothstep per fragment -- five times
 * over under --sharpen.
 */
float purkinjeGlobalWeight() {
    float logLada = texelFetch(purkinjeAdaptTex, ivec2(0, 0), 0).r;
    /*
     * Two ways the reading can be untrustworthy, and both fall back to 1.0 --
     * the local gate alone -- rather than to 0.0. A flag that silently does
     * nothing is worse than a degraded picture.
     *
     * No meter: the C side ran no measure draws, and an unwritten R32F target's
     * content is undefined. The shader cannot know that, so it is told. This is
     * live for the DEBUG VIEW, which evaluates the weight with the feature off;
     * the enable gate in purkinjeApply already implies a meter on every other
     * path.
     *
     * NaN: lum_reduce_frag writes a DELIBERATE NaN on an empty histogram, which
     * spot metering at a small radius reaches. It matters because NaN survives
     * the `w <= 0.0` guard -- that test is FALSE on NaN -- so the identity's own
     * early-out is exactly what a NaN would slip past, into a NaN frame.
     */
    if (purkinjeHasMeter == 0 || isnan(logLada))
        return 1.0;
    // Edges LO < HI always: smoothstep with edge0 > edge1 is undefined in GLSL,
    // and the frame it produces is plausible rather than obviously broken.
    return 1.0 - smoothstep(PK_GLOBAL_LO + purkinjeBiasEV,
                            PK_GLOBAL_HI + purkinjeBiasEV, logLada);
}

// The PER-PIXEL gate: is there light HERE for the cones to answer. `c` is finite
// by construction -- the caller sanitized it against WS_SCENE_MAX -- which is
// what the exact identity rests on.
float purkinjeLocalWeight(vec3 c) {
    float lumPix = dot(c, vec3(0.2126, 0.7152, 0.0722)) * oneOverPreExposure;
    // The same 1e-8 guard lum_measure_frag uses, so both agree about black.
    float logLpix = log2(max(lumPix, 1.0e-8));
    return 1.0 - smoothstep(PK_LOCAL_LO + purkinjeBiasEV,
                            PK_LOCAL_HI + purkinjeBiasEV, logLpix);
}

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
 * Returns (w, wLocal, wGlobal). The debug view wants all three, and they are the
 * factors of the first, so packing them costs nothing over returning w alone.
 *
 * Strength scales the WEIGHT and nothing else. Folding it into the tint instead
 * (mix(vec3(1), TINT, strength)) still desaturates fully at w = 1, so strength 0
 * would drain colour while looking, in code, like a control.
 */
vec3 purkinjeWeight(vec3 c) {
    float wGlobal = purkinjeGlobalWeight();
    float wLocal = purkinjeLocalWeight(c);
    return vec3(purkinjeStrength * wLocal * wGlobal, wLocal, wGlobal);
}

// How far rod pooling smears detail at this weight, in texels. The caller owns
// the resampling -- it is the scene's business, not the retina's -- so this is
// the only part of acuity that belongs here.
float purkinjePoolRadius(float w) {
    return PK_ACUITY_TEXELS * purkinjeAcuity * w;
}

/*
 * Apply the spectral shift. A BLEND toward the rod image, not Kirk & O'Brien's
 * additive rod-plus-attenuated-cone sum: an additive term adds energy, which
 * brightens a night frame and fights both the WS_* ceilings and the tone curve's
 * shoulder -- and mix(c, x, 0.0) is EXACTLY c, which the additive form is not.
 *
 * `c` arrives already pooled by the caller when acuity is on, so this is purely
 * the retina's spectral response plus its noise. No sampler, no texel size, no
 * composite terms: everything spatial belongs to whoever owns the scene.
 */
vec3 purkinjeShift(vec3 c, float w, vec2 uv, float seed) {
    c = mix(c, dot(c, PURKINJE_SCOTOPIC_W) * PURKINJE_ROD_TINT, w);

    /*
     * ROD NOISE, and it is a different thing from the grain stage downstream.
     * Grain is SENSOR noise laid over a finished look, which is why it sits
     * after the display encode. This is RETINAL: photon arrivals are Poisson, so
     * a dim scene is genuinely noisy to the eye before any transfer curve, and
     * it scales with how far into rod vision the pixel is.
     *
     * Rides the grain stage's own frame seed rather than carrying a second one:
     * that value is already deterministic across equal --frames runs, and a
     * third source of per-frame randomness is a third thing to keep that way.
     * The offset keeps the two fields from correlating where their arguments
     * would otherwise be close.
     *
     * Multiplicative, so it vanishes in true black rather than lifting it -- a
     * night frame is half at the radiance floor, and additive noise there would
     * print static onto pixels that carry no light at all.
     */
    if (purkinjeNoise > 0.0) {
        float n = hash21(uv * 1024.0 + seed + 37.0, vec2(12.9898, 78.233)) - 0.5;
        c *= 1.0 + n * PK_NOISE_AMOUNT * purkinjeNoise * w;
    }
    return c;
}
