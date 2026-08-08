// Moment-based order-independent transparency (Munstermann et al., I3D 2018),
// four power moments. Spec 11.17.
//
// Weighted-blended OIT guesses how much of a fragment survives the layers in
// front of it from a curve fitted to depth alone. This measures it instead. Pass
// 1 accumulates, additively and order-independently, a five-number summary of
// the absorbance along each pixel's depth axis: the total b0 and its first four
// moments in a warped depth. Pass 2 reconstructs the absorbance in front of each
// fragment from that summary and weights its colour by the transmittance.
//
// Power moments, not trigonometric: the trigonometric variant needs complex
// arithmetic and every shader here is #version 330 core.
//
// Split out so the two passes cannot disagree about a constant -- the warp
// especially, which is the coordinate the moments are moments OF -- and so the
// atlas layout has one home instead of being restated wherever it is touched.
// Only pbr_frag includes it today; both passes live there. Same reason
// preintegrated_skin.glsl is shared (spec 11.13).

// Depth warp: linear view depth -> [-1, 1], logarithmically. Logarithmic
// because a moment is a weighted average and a linear parameterisation would
// spend its whole resolution on the far field, where a scene's transparency
// almost never is. The [-1, 1] range is what MBOIT_BIAS_VECTOR below is stated
// over -- change one and the other stops meaning anything.
//
// The camera's own near/far, deliberately, rather than a range fitted to the
// transparent geometry. Fitting it was tried: on the card fixture it moves the
// warped span from 39% of the interval to all of it and the error does not
// budge (0.01577 against 0.01565 RMS). What limits the reconstruction is how
// many moments there are, not how much of the axis they cover.
float mboitWarpDepth(float viewZ, vec2 nearFar) {
    float t = log(max(viewZ, nearFar.x) / nearFar.x) / log(nearFar.y / nearFar.x);
    return clamp(t, 0.0, 1.0) * 2.0 - 1.0;
}

// Absorbance of a layer of opacity a: the optical depth whose transmittance is
// (1 - a). Summing THESE is what makes the accumulation order-independent --
// transmittance multiplies, absorbance adds, and only the second can ride a
// GL_ONE/GL_ONE blend. The clamp keeps a fully opaque fragment from writing
// infinity into the buffer.
float mboitAbsorbance(float a) {
    return -log(1.0 - clamp(a, 0.0, 0.9999));
}

// The four power moments of one layer, for the additive generation pass. b0 (the
// total) is written separately -- see the atlas note in pbr_frag.
vec4 mboitMoments(float absorbance, float z) {
    float z2 = z * z;
    return absorbance * vec4(z, z2, z2 * z, z2 * z2);
}

// Regularisation. The moment problem is solved through a Cholesky factorisation
// of a Hankel matrix built from the moments, and that matrix is singular exactly
// when the depth distribution is degenerate -- one layer, or several at one
// depth, which is the common case rather than the pathological one. Blending the
// moments a hair toward a fixed well-conditioned vector keeps it definite.
//
// Both are the reference's, at single precision. The vector is NOT the moments
// of a uniform distribution on the warp range (those would be 0, 1/3, 0, 1/5) --
// it is a conditioning constant chosen against that range, so it and the [-1, 1]
// warp above only mean anything together. The targets are fp32 for the same
// reason this has to exist at all.
#define MBOIT_BIAS        5.0e-7
#define MBOIT_BIAS_VECTOR vec4(0.0, 0.375, 0.0, 0.375)
// Fraction of a fragment's OWN layer counted as being in front of it.
//
// Exact alpha compositing says zero -- a layer is not attenuated by itself --
// and zero is by far the worst value available. What the reconstruction returns
// is a LOWER bound on the absorbance in front, so it under-occludes by
// construction, and this term is what pays that back. Measured on the card
// fixture: the reference's quarter lands at 0.0157 RMS against the arithmetic,
// zero at 0.0674 -- worse than the depth curve it replaces.
#define MBOIT_OVERESTIMATION 0.25

// Transmittance in front of `z`, from the total absorbance b0 and the four power
// moments b1..b4. Munstermann et al.'s four-power-moment reconstruction,
// transcribed: factor the Hankel matrix, solve for the polynomial that
// interpolates the step function at the fragment's depth and the two roots the
// moments imply, then take its expectation under the moments.
float mboitTransmittance(float b0, vec4 moments, float z) {
    if (b0 <= 0.0)
        return 1.0;
    vec4 b = mix(moments / b0, MBOIT_BIAS_VECTOR, MBOIT_BIAS);

    // Cholesky of the Hankel matrix, keeping only the non-trivial entries
    float L21D11 = b[2] - b[0] * b[1];
    float D11 = b[1] - b[0] * b[0];
    float invD11 = 1.0 / D11;
    float L21 = L21D11 * invD11;
    float D22 = (b[3] - b[1] * b[1]) - L21D11 * L21;

    // Scaled inverse image of (1, z, z^2): forward substitution, scale, back
    // substitution
    vec3 c = vec3(1.0, z, z * z);
    c[1] -= b[0];
    c[2] -= b[1] + L21 * c[1];
    c[1] *= invD11;
    c[2] /= D22;
    c[1] -= L21 * c[2];
    c[0] -= dot(c.yz, b.xy);

    // The two depths the moments place the rest of the mass at: roots of
    // c0 + c1*z + c2*z^2
    float invC2 = 1.0 / c[2];
    float p = c[1] * invC2;
    float q = c[0] * invC2;
    float r = sqrt(max(p * p * 0.25 - q, 0.0));
    float z1 = -p * 0.5 - r;
    float z2 = -p * 0.5 + r;

    // Newton form of the quadratic through (z, overestimation), (z1, in front?),
    // (z2, in front?), expanded to coefficients
    float f0 = MBOIT_OVERESTIMATION;
    float f1 = z1 < z ? 1.0 : 0.0;
    float f2 = z2 < z ? 1.0 : 0.0;
    float f01 = (f1 - f0) / (z1 - z);
    float f12 = (f2 - f1) / (z2 - z1);
    float f012 = (f12 - f01) / (z2 - z);
    float c1 = f01 - f012 * z1; // Newton coefficient, before the shift to z
    vec3 poly;
    poly[2] = f012;
    poly[1] = c1 - f012 * z;
    poly[0] = f0 - c1 * z;

    // Expectation of that polynomial under the moments IS the absorbance in
    // front, normalised; b0 puts it back on its own scale
    float absorbance = poly[0] + dot(b.xy, poly.yz);
    return clamp(exp(-b0 * absorbance), 0.0, 1.0);
}

// The generation pass resolves its two multisample targets into ONE texture of
// twice the render height: b1..b4 in the lower half, b0 in the upper. That
// layout is stated here and read back here, so the tap offset and the reciprocal
// size cannot drift apart; the blit that produces it is engine_end_moment_pass.
#define MBOIT_ATLAS_B0_TAP vec2(0.0, 0.5)

// Transmittance in front of this fragment, measured off the atlas rather than
// guessed from its depth. invAtlasSize is the reciprocal of the ATLAS, so its y
// is half the frame's.
float mboitTransmittanceAtlas(sampler2D atlas, vec2 invAtlasSize, vec2 nearFar, float viewZ) {
    vec2 uv = gl_FragCoord.xy * invAtlasSize;
    return mboitTransmittance(textureLod(atlas, uv + MBOIT_ATLAS_B0_TAP, 0.0).r,
                              textureLod(atlas, uv, 0.0),
                              mboitWarpDepth(viewZ, nearFar));
}
