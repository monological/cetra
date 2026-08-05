#version 330 core
#include "sss_profile.glsl"

in vec2 TexCoords;
out vec4 FragColor;

// Read the scatter profile out of the pyramid: one trilinear tap per Gaussian
// per channel, at a level where that Gaussian's sigma is a couple of texels.
//
// This is the pass that removes the tap budget from the picture. A kernel
// sampled at discrete taps shows those taps as rings once it is wide enough in
// PIXELS, and the width in pixels is exactly what has to grow when the delivered
// width in WORLD units is held constant across resolutions. Pre-filtering breaks
// that tie: reach is bounded by the level count, not by how many samples the
// shader can afford, so nine taps here deliver what no number of taps could
// deliver by marching the profile directly.
//
// The shape is unchanged. This evaluates the same three Gaussians the separable
// blur did, at the same relative widths, so gen_skin_preint_fit.py's table still
// describes what runs.

uniform sampler2D pyrColor; // rgb = coverage-premultiplied diffuse, a = coverage
uniform sampler2D origTex;  // resolved attachment 4: the sharp diffuse D
uniform sampler2D auxTex;   // .z = linear view Z, negative in front

uniform vec4 sssProfile;  // rgb = per-channel scatter weight, w = world radius
uniform int profileTag;   // this walk's profile index + 1
uniform float projScale;  // 0.5 * proj[1][1] * renderHeight
uniform float maxLod;     // coarsest level the gather may read (see lodCap, postfx.c)
uniform vec2 renderTexel; // one RENDER-res texel, for sizing a level's own texel

// Pre-filter sigma of level L, in RENDER pixels, in closed form.
//
// One 13-tap step contributes variance 0.4375 in its DESTINATION texels -- the
// point taps carry sum(w * offset^2) = 1.5 source texels squared, each of the
// four bilinear taps is an exact 2x2 box worth 0.25, and a destination texel is
// two source texels across. Accumulating var_L = 0.4375 + var_{L-1} / 4 from
// zero gives var_L = 0.5833 * (1 - 4^-L), and level L's texels are 2^L render
// pixels wide.
//
// Closed form rather than a uniform array on purpose: an array upload that
// silently fails to bind leaves every sigma at zero, which sends every term to
// the coarsest level and flattens the subject into its own average. That failed
// exactly that way here, and a value the shader derives cannot fail to arrive.
//
// The (1 - 4^-L) factor is not negligible at the bottom: level 1 is 13% below
// the 0.7638 asymptote, and it is the level carrying the sharpest detail.
float levelSigmaPx(float L) {
    return exp2(L) * sqrt(0.5833333 * (1.0 - exp2(-2.0 * L)));
}

// A level's texels are 2^lod render texels across, and at the LODs the wide
// terms need that can be a sixth of the object. One point tap there samples the
// level's own grid, which reads as facets with hard edges -- the mip chain's
// version of the tap rings this pass exists to remove.
//
// Four taps on the level's half-texel diagonal turn that piecewise-bilinear
// surface into a tent, which is smooth across texel boundaries. It is the same
// reason the bloom chain upsamples with a tent instead of point-sampling a
// coarse level, applied at sample time because this LOD is chosen per pixel and
// so cannot be baked into a fixed upsample walk.
vec4 tentLod(sampler2D tex, vec2 uv, float lod) {
    vec2 h = 0.5 * exp2(lod) * renderTexel;
    return 0.25 * (textureLod(tex, uv + vec2(-h.x, -h.y), lod) +
                   textureLod(tex, uv + vec2(h.x, -h.y), lod) +
                   textureLod(tex, uv + vec2(-h.x, h.y), lod) +
                   textureLod(tex, uv + vec2(h.x, h.y), lod));
}

// Invert the trilinear mix exactly rather than assuming a level's sigma doubles.
//
// LINEAR_MIPMAP_LINEAR returns (1-f) * L + f * (L+1), whose VARIANCE is the
// same mix of the two levels' variances -- so matching the second moment means
// interpolating in sigma^2, not in sigma. Solving in sigma instead (the obvious
// log2(sigma / sigma1)) comes out up to 12% too wide at f = 0.5, because a
// mixture of G(s) and G(2s) has an effective sigma of 1.58s where 1.41s was
// asked for. A width error that varies WITH the level is a visible step at every
// level boundary, which is the artifact this whole pass exists to remove.
float lodForSigma(float sigmaPx, float lodCap) {
    // Below level 1's own width there is nothing to interpolate toward: level 0
    // is unfiltered, so fade in linearly rather than taking log2 of a ratio
    // under 1.
    float s1 = levelSigmaPx(1.0);
    if (sigmaPx <= s1)
        return min(sigmaPx / s1, lodCap);
    // Asymptotic guess, then one correction against the exact form. Sigma is
    // very nearly 0.7638 * 2^L, so the guess is within a few percent by level 2
    // and the correction removes the rest -- including the 13% error at level 1,
    // where assuming the asymptote would mis-size the sharpest term.
    float L = log2(sigmaPx / 0.7637626);
    L += log2(sigmaPx / max(levelSigmaPx(max(L, 1.0)), 1e-6));
    // Floored at 1.0 so the two branches MEET. The fade-in reaches exactly 1.0
    // at s1 while this one lands at 0.79 there, and an unfloored 0.21-LOD jump
    // at the seam is a step in delivered width -- the same class of artifact the
    // exact inversion exists to avoid.
    return min(max(L, 1.0), lodCap);
}

void main()
{
    vec4 center = texture(origTex, TexCoords);
    float z = texture(auxTex, TexCoords).z;
    // Off-skin and sky contribute exactly nothing, alpha included, so the
    // additive fold provably cannot paint outside the subject.
    if (z >= 0.0 || int(center.a + 0.5) != profileTag) {
        FragColor = vec4(0.0);
        return;
    }

    float depth = -z;
    vec3 masses = sssProfileMasses();
    // Base width in render pixels, from the authored world radius. The pixel
    // count is free to grow with resolution now; only the world width is fixed.
    float basePx = sssProfile.w * projScale / depth;

    // Apply the ceiling to the BASE radius, not to each channel's sigma.
    //
    // Red scatters widest, so a per-channel clamp bites red first and leaves
    // green and blue untouched -- which compresses exactly the ratio that makes
    // skin redden rather than grey out. Measured against the separable blur it
    // visibly desaturated the terminator. Clamping the base keeps red:green:blue
    // exact through the ceiling, the same reason skinSigma clamps its base rather
    // than its channels (§11.13 D6).
    float peak = max(max(sssProfile.r, sssProfile.g), sssProfile.b);
    float widestMult = SSS_PROFILE_MULT.z * max(peak, 1e-3);
    basePx = min(basePx, levelSigmaPx(maxLod) / widestMult);

    vec3 num = vec3(0.0);
    vec3 den = vec3(0.0);
    for (int term = 0; term < 3; term++) {

        // Depth rejection lives ONLY in the downsample, where it compares texels
        // one apart at that level. A second guard here, against the level's MEAN
        // depth, was tried and removed: on a curved surface the mean over a
        // term's reach is displaced from the local depth by roughly the reach
        // itself, so it rejects hardest exactly where the surface turns away --
        // the terminator, which is the neighbourhood the scatter is supposed to
        // come from. It replaced the soft red falloff with a hard dark edge,
        // because suppressing the lit side leaves blur BELOW the sharp diffuse
        // and the composite then subtracts.
        //
        // What that guard was meant to catch -- a coarse texel whose mean sits
        // on a different surface, a hand in front of a face -- is NOT caught by
        // anything today. Coverage cannot see it: two surfaces of the same
        // profile are both fully covered. And the downsample's own tolerance is
        // floored at the authored radius, which dominates its level term for the
        // first several levels, so it does not catch it either. A real gap, with
        // no case for it in any fixture. Spec 11.14 records what a fix looks
        // like: reject on a per-texel depth SPREAD rather than a mean.
        float sigmaBase = SSS_PROFILE_MULT[term] * basePx;
        float mass = masses[term];
        for (int c = 0; c < 3; c++) {
            float lod = lodForSigma(sigmaBase * sssProfile[c], maxLod);
            // Below level 1 there is no texel grid to smooth: level 0 is
            // unfiltered and its texels are single render pixels, so the tent
            // would be four fetches averaging what hardware bilinear already
            // gives. The core term sits here at almost every framing.
            vec4 t = lod < 1.0 ? textureLod(pyrColor, TexCoords, lod)
                               : tentLod(pyrColor, TexCoords, lod);
            num[c] += mass * t[c];
            den[c] += mass * t.a;
        }
    }

    // Divide by accumulated COVERAGE, not by the weight sum: near a silhouette a
    // coarse texel is only partly skin, and the premultiplied numerator already
    // carries that same partial weight. Dividing by weight alone would darken the
    // rim toward the uncovered black; dividing by coverage renormalises to the
    // skin that is actually there, which is the pyramid's analogue of the
    // separable pass's sum/sumW.
    // Where nothing survived the guards there is no scatter to report, so the
    // answer is the sharp diffuse itself -- a delta of zero. Dividing by a
    // floored denominator instead would return zero and make the composite
    // SUBTRACT the diffuse, which is the worst available answer and is what a
    // too-tight depth guard used to produce across whole surfaces.
    vec3 blur = mix(center.rgb, num / max(den, vec3(1e-5)),
                    smoothstep(vec3(0.0), vec3(1e-3), den));

    // The composite is hdr + blur - D, folded additively. Alpha 0 so the fold
    // leaves canvas alpha untouched.
    FragColor = vec4(blur - center.rgb, 0.0);
}
