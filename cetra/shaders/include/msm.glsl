// Moment shadow maps (Peters & Klein, HPG 2015), four power moments. Spec 11.22.
//
// A depth compare is a step function of the stored depth, so averaging depths and
// then comparing is NOT the average of the comparisons -- which is why a depth map
// cannot be downsampled or blurred, and why widening the PCF kernel is the only
// lever a binary tap has. Storing the first four moments of the depth distribution
// moves the comparison into a quantity that averages: the map can then be
// downsampled, blurred and sampled bilinearly, and a single tap answers what PCSS
// spends 32 stochastic taps estimating.
//
// Power moments, not trigonometric: the trigonometric variant needs complex
// arithmetic and every shader here is #version 330 core. Same call and same reason
// as include/mboit.glsl, which is the same family of technique applied to
// transparency rather than occlusion.

// Map [0,1] shadow-map depth onto the range the bias vector below is stated over.
//
// Linear, where mboit.glsl's warp is logarithmic, and the difference is the
// distribution rather than a preference: a cascade's orthographic depth is already
// uniform across the box it was fitted to, while transparency clusters near the
// camera and needs its resolution spent there.
float msmWarpDepth(float d01) {
    return clamp(d01, 0.0, 1.0) * 2.0 - 1.0;
}

// The four power moments of one depth sample, in the warped coordinate.
//
// These must be formed per TAP and then averaged, never the other way round --
// forming them from an averaged depth is the step-function mistake above, and it
// is what makes a 2x2 downsample legal here and illegal on the depth map.
vec4 msmMoments(float z) {
    float z2 = z * z;
    return vec4(z, z2, z2 * z, z2 * z2);
}

// Regularisation. The reconstruction below factorises a Hankel matrix built from
// the moments, and that matrix is singular exactly when the depth distribution is
// degenerate -- for a shadow map, every texel covered by a single flat surface,
// which is the common case rather than the pathological one. Blending the moments a
// hair toward a fixed well-conditioned vector keeps it definite.
//
// The vector is the same one mboit.glsl carries and is only meaningful against the
// [-1,1] warp above -- change one and the other stops meaning anything. Both
// magnitudes are their own paper's reference constant, Peters' here and
// Munstermann's there; the difference between them is not derived from anything
// in this codebase, so do not reason from it.
#define MSM_BIAS        3.0e-5
#define MSM_BIAS_VECTOR vec4(0.0, 0.375, 0.0, 0.375)

// Fraction of the light at warped depth `z` that is occluded, from the four moments.
//
// Peters' Hamburger 4MSM: factor the Hankel matrix, solve for the quadratic whose
// roots are the two depths the moments place the distribution's mass at, then read
// the occlusion off which side of the fragment those roots fall.
//
// The factorisation and the root solve are the same arithmetic as
// mboitTransmittance's, but NOT the same operation order: this follows Peters in
// keeping the third pivot as the product D33D22 and applying it as
// `c[2] *= D22 / D33D22`, where mboit follows Munstermann in dividing through
// first. Same number, different expression. Only the last step genuinely differs,
// where this does a case analysis on the roots and that takes an expectation
// under the moments.
//
// Sharing the kernel with mboit was measured and rejected (spec 11.22); an
// earlier version of this comment proposed it as a follow-up and understated
// what it costs. Two facts decide it. The regularisation constants are 60x
// apart and each is its own paper's reference value, and mboit normalises its
// moments by a total absorbance this side has no counterpart for -- so a shared
// helper takes both as parameters, which is a wrapper around 18 lines rather
// than an abstraction. And the two orderings above cannot both survive: one
// side's rounding has to move.
//
// The retraction worth carrying: that proposal argued the moment shadow path
// had no baseline to protect. It has six analytic gate arms measuring this
// reconstruction against numeric tolerances, which is exactly what reshuffling
// this arithmetic disturbs. A baseline need not be a stored image.
float msmOcclusion(vec4 moments, float z) {
    vec4 b = mix(moments, MSM_BIAS_VECTOR, MSM_BIAS);

    // Cholesky of the Hankel matrix, keeping the third pivot as a product.
    float L32D22 = b[2] - b[0] * b[1];
    float D22 = b[1] - b[0] * b[0];
    float squaredDepthVariance = b[3] - b[1] * b[1];
    float D33D22 = squaredDepthVariance * D22 - L32D22 * L32D22;
    float invD22 = 1.0 / D22;
    float L32 = L32D22 * invD22;

    // Scaled inverse image of (1, z, z^2): forward substitution, scale, back
    // substitution.
    vec3 c = vec3(1.0, z, z * z);
    c[1] -= b[0];
    c[2] -= b[1] + L32 * c[1];
    c[1] *= invD22;
    c[2] *= D22 / D33D22;
    c[1] -= L32 * c[2];
    c[0] -= dot(c.yz, b.xy);

    // The two depths the moments place the mass at: roots of c0 + c1*z + c2*z^2.
    float invC2 = 1.0 / c[2];
    float p = c[1] * invC2;
    float q = c[0] * invC2;
    float r = sqrt(max(p * p * 0.25 - q, 0.0));
    float z1 = -p * 0.5 - r;
    float z2 = -p * 0.5 + r;

    // Both roots nearer the light than the fragment means it is behind everything
    // the moments know about; both farther means nothing is in front of it. The
    // quotient interpolates the case where the fragment falls between them.
    vec4 sw = (z2 < z) ? vec4(z1, z, 1.0, 1.0)
                       : ((z1 < z) ? vec4(z, z1, 0.0, 1.0) : vec4(0.0));
    float quotient =
        (sw[0] * z2 - b[0] * (sw[0] + z2) + b[1]) / ((z2 - sw[1]) * (z - z1));
    return clamp(sw[2] + sw[3] * quotient, 0.0, 1.0);
}

// Remap the low end of the occlusion range to zero.
//
// What the reconstruction returns is a LOWER bound on the occlusion, so it
// under-occludes by construction and a fully shadowed texel can read as slightly
// lit -- the light leak specs/10.4 named as this family's signature failure, and
// the reason it declined VSM. This is Peters' own cure. It is a knob and not a
// constant because how much of the range is leak depends on how degenerate the
// depth distribution inside a texel is, which is a property of the scene's
// geometry rather than of the algorithm.
float msmReduceBleed(float occlusion, float amount) {
    return clamp((occlusion - amount) / (1.0 - amount), 0.0, 1.0);
}
