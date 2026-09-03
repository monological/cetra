// Pre-integrated skin diffuse (Penner 2011), as a closed form.
//
// On a curved surface, light entering at one point leaves at another, so the
// diffuse response at a given angle is the clamped cosine convolved with the
// material's diffusion profile over the chord between them. Evaluating that
// integral per pixel is out of the question; this is the fit.
//
// Constants come from tools/gen_skin_preint_fit.py, which integrates the
// response against the SAME profile include/sss_profile.glsl defines, so the
// angular and screen-space halves of subsurface cannot disagree about the shape
// of skin. Nothing in the build enforces that coupling: if profileWeight()
// changes, re-run the tool's default (verify) mode, which reports the error of
// the table below against a fresh integration.
//
// Measured for this table: max 0.028, rms 0.008, monotone in NdotL, energy
// error 0.033. Above sigma 0.6 the error is under 0.011. The worst case sits at
// sigma 0.08, which is also where the effect being modelled is smallest.
//
// sigma is dimensionless: sssColor_c * sssRadius * curvature, per channel. The
// reduction is exact rather than small-angle -- substituting the true chord
// 2 sin(x/2) makes the radius cancel identically.

#include "sss_profile.glsl"

#define SKIN_TABLE_N   16
#define SKIN_SIGMA_MAX 2.0

// Wrap and the clamped cubic: (w, a1, a2, a3).
const vec4 SKIN_SHAPE_A[SKIN_TABLE_N] = vec4[SKIN_TABLE_N](
    vec4(+0.000000, +1.000000, +0.000000, +0.000000),  // sigma 0.0000
    vec4(+0.002937, +0.982777, +0.036989, -0.020117),  // sigma 0.0089
    vec4(+0.014651, +0.909094, +0.180625, -0.095951),  // sigma 0.0356
    vec4(+0.042966, +0.739358, +0.448593, -0.223398),  // sigma 0.0800
    vec4(+0.296181, +0.000002, +1.676619, -0.743188),  // sigma 0.1422
    vec4(+0.370892, +0.000001, +1.299864, -0.487884),  // sigma 0.2222
    vec4(+0.428386, -0.000001, +0.921226, -0.299382),  // sigma 0.3200
    vec4(+0.477456, -0.000001, +0.630532, -0.178147),  // sigma 0.4356
    vec4(+0.513038, -0.000000, +0.443553, -0.111772),  // sigma 0.5689
    vec4(+0.513773, +0.000000, +0.321250, -0.080772),  // sigma 0.7200
    vec4(+0.502463, -0.000000, +0.240158, -0.062827),  // sigma 0.8889
    vec4(+0.511037, -0.000000, +0.187660, -0.047886),  // sigma 1.0756
    vec4(+0.550399, -0.000001, +0.153151, -0.034116),  // sigma 1.2800
    vec4(+0.615359, -0.000000, +0.128879, -0.021986),  // sigma 1.5022
    vec4(+0.690852, -0.000000, +0.110138, -0.012613),  // sigma 1.7422
    vec4(+0.761687, +0.000001, +0.094044, -0.006485)   // sigma 2.0000
);

// The two terms that live over the WHOLE sphere: (d0, e0).
//
// d0 is a floor. Because the kernel works on the chord it has support
// everywhere, so a surface facing directly AWAY from the light still returns
// something -- 0.0117 at sigma 0.3, tending to 1/pi as sigma grows. A clamped
// wrap is exactly zero past its support and cannot express that at all.
// e0 is an unclamped fundamental. The response is 1/pi + (m1/2) cos(theta) plus
// harmonics; only the harmonics concentrate near the terminator, so without e0
// the fit has to fake cos(theta) with a term that is zero over half the domain.
const vec2 SKIN_SHAPE_B[SKIN_TABLE_N] = vec2[SKIN_TABLE_N](
    vec2(+0.000000, +0.000000),  // sigma 0.0000
    vec2(-0.008114, +0.000003),  // sigma 0.0089
    vec2(-0.006599, +0.001883),  // sigma 0.0356
    vec2(+0.002926, +0.013395),  // sigma 0.0800
    vec2(+0.007737, +0.017651),  // sigma 0.1422
    vec2(+0.046116, +0.056531),  // sigma 0.2222
    vec2(+0.107266, +0.100139),  // sigma 0.3200
    vec2(+0.162334, +0.112379),  // sigma 0.4356
    vec2(+0.201927, +0.110250),  // sigma 0.5689
    vec2(+0.232518, +0.107509),  // sigma 0.7200
    vec2(+0.253784, +0.099107),  // sigma 0.8889
    vec2(+0.266279, +0.085433),  // sigma 1.0756
    vec2(+0.272962, +0.071102),  // sigma 1.2800
    vec2(+0.276624, +0.059246),  // sigma 1.5022
    vec2(+0.279306, +0.051065),  // sigma 1.7422
    vec2(+0.282269, +0.046447)   // sigma 2.0000
);

// Per-channel shape, resolved once per fragment. sigma never depends on the
// light, so resolving it per light would pay the lookup for nothing.
struct SkinShape {
    vec3 w;
    vec3 a1;
    vec3 a2;
    vec3 a3;
    vec3 d0;
    vec3 e0;
};

// Surface curvature from the GEOMETRIC normal, in inverse world units.
//
// Ng must be the interpolated vertex normal, never the normal-mapped one: the
// shading normal's derivative is a texture-detail metric (pbr_frag uses exactly
// that to widen roughness for specular AA), and since sigma scales linearly in
// curvature, millimetre normal detail would drive every skin pixel past the flat
// response and dissolve the terminator entirely.
//
// The RMS form rather than the more obvious length(fwidth(N))/length(fwidth(P)):
// fwidth is a componentwise abs-sum, so that ratio carries a frame-orientation
// bias of up to 40% on anisotropic curvature. This is exactly 1/R on a sphere at
// any screen orientation, and reuses world derivatives the caller already has.
//
// A flat-shaded triangle has a constant normal, so this returns exactly 0 there
// -- in the interior AND in partially covered edge quads, since a derivative
// quad only ever interpolates one primitive. Hard edges are a no-op, not noise.
// The real noise source is tessellation: the normal is only C0 across triangle
// boundaries, so a quad straddling one sees a large dN over a small dP.
float skinCurvature(vec3 Ng, vec3 ddxW, vec3 ddyW) {
    vec3 dNx = dFdx(Ng);
    vec3 dNy = dFdy(Ng);
    float dP2 = dot(ddxW, ddxW) + dot(ddyW, ddyW);
    return sqrt((dot(dNx, dNx) + dot(dNy, dNy)) / max(dP2, 1e-12));
}

// One channel's row, lerped. Index is quadratic in sigma so the inverse is a
// single sqrt and the entries bunch where the coefficients actually move;
// uniform spacing would spend half of them above sigma 1 where the curves are
// nearly flat.
void skinShapeChannel(float sigma, out vec4 a, out vec2 b) {
    float f = sqrt(clamp(sigma, 0.0, SKIN_SIGMA_MAX) / SKIN_SIGMA_MAX) *
              float(SKIN_TABLE_N - 1);
    int i = min(int(f), SKIN_TABLE_N - 2);
    float frac = f - float(i);
    a = mix(SKIN_SHAPE_A[i], SKIN_SHAPE_A[i + 1], frac);
    b = mix(SKIN_SHAPE_B[i], SKIN_SHAPE_B[i + 1], frac);
}

// Resolve all three channels. sigma = 0 lands on row 0 with frac 0, and mix()
// at 0 returns its first argument exactly, so the shape comes back as
// (w, a1, ...) = (0, 1, 0, 0, 0, 0) BIT-EXACTLY -- which is what makes
// skinDiffuse reduce to max(NdotL, 0) and the caller's delta exactly +0.0.
SkinShape skinShape(vec3 sigma) {
    vec4 ar, ag, ab;
    vec2 br, bg, bb;
    skinShapeChannel(sigma.r, ar, br);
    skinShapeChannel(sigma.g, ag, bg);
    skinShapeChannel(sigma.b, ab, bb);
    SkinShape s;
    s.w = vec3(ar.x, ag.x, ab.x);
    s.a1 = vec3(ar.y, ag.y, ab.y);
    s.a2 = vec3(ar.z, ag.z, ab.z);
    s.a3 = vec3(ar.w, ag.w, ab.w);
    s.d0 = vec3(br.x, bg.x, bb.x);
    s.e0 = vec3(br.y, bg.y, bb.y);
    return s;
}

// The per-light half: a wrap, a cubic, and the two global terms.
//
// ndotl is the UNCLAMPED cosine. The whole point is the response below zero,
// and pbr_frag's own NdotL is clamped there.
//
// No pow(): pow(x, 1.0) is not guaranteed exact to an ulp and pow(0, 1) is
// undefined in GLSL, either of which would cost the bit-exact identity above.
// The outer max() is not defensive. The fit's objective penalises squared
// error, monotonicity and energy -- never SIGN -- and rows 1 to 5 come back with
// d0 < e0, so D(-1) = d0 - e0 lands around -0.010. The true integral is a
// non-negative kernel against a clamped cosine and cannot be negative anywhere.
// Because sigma is per-channel and skin profiles weight red widest, it is green
// and blue that sit in that band at ordinary curvatures: without this the far
// side of a lit head SUBTRACTS green and blue from whatever fill reaches it, and
// what looks like reddening is partly the other two channels being removed. It
// clips to black in an ambient-free fixture, which is why the golden cannot see
// it. Costs nothing at sigma 0: max(max(x,0), 0) is max(x,0).
vec3 skinDiffuse(SkinShape s, float ndotl) {
    vec3 t = max((vec3(ndotl) + s.w) / (vec3(1.0) + s.w), vec3(0.0));
    return max(s.d0 + s.e0 * ndotl + t * (s.a1 + t * (s.a2 + t * s.a3)), vec3(0.0));
}

// The angular width this fragment scatters over, per channel.
//
// `deficit` is why pre-integration and the screen-space blur do not double
// count. The blur's pyramid stops being sampled while its texels are still small
// against the subject, so past a certain closeness it delivers less than the
// authored radius; `maxScatterPerDepth` is what it can still reach, reported by
// postfx_sss_max_sigma_per_depth. This takes up the slack: where the blur
// delivers the authored width the deficit is 0 and pre-integration contributes
// NOTHING.
vec3 skinSigma(vec3 scatterColor, float scatterRadius, float curvature,
               float curvatureScale, float maxScatterPerDepth, float clipW) {
    // maxScatterPerDepth is the widest SIGMA the screen-space pass can deliver,
    // so convert it back to the base radius that sigma came from before
    // comparing: the widest term is the tail multiplier times the reddest
    // channel times the radius. Comparing a base radius against a tail-scale
    // ceiling would report the pass as complete while its longest reach was
    // still being cut.
    float peakColor = max(max(scatterColor.r, scatterColor.g), scatterColor.b);
    float reachToBase = 1.0 / max(SSS_PROFILE_MULT.z * peakColor, 1e-3);
    float want = scatterRadius;
    // maxScatterPerDepth is per unit of CLIP W, which is the depth under a
    // perspective camera and exactly 1 under an orthographic one -- the caller
    // passes clipWAt(viewZ) rather than -viewZ so this chunk need not know which.
    float got = min(want, maxScatterPerDepth * reachToBase * clipW);
    float k = got / max(want, 1e-6);
    // The shortfall SQUARED, not sqrt(1 - k^2).
    //
    // The sqrt form assumed variances add, which requires a unit of angular
    // sigma to be worth a unit of screen-space sigma. Measured against Penner's
    // integral it is not, and the error is large: with the pyramid delivering
    // k = 0.24 of the authored radius, sqrt(1 - k^2) is 0.97 and the total lands
    // at 269% of what the integral asks for. 11.13 recorded the two halves as
    // not interchangeable currencies but could not see the size of it, because
    // the blur was then delivering enough that the deficit stayed near zero.
    //
    // (1 - k)^2 lands the total at 126% of the reference, which is where the
    // separable blur sat before this branch (129%) -- so the look is held against
    // ground truth while the resolution defect is fixed. It is a fitted exponent,
    // not a derivation, and it is fitted to one fixture; that is why it is
    // written as the shortfall rather than dressed up as a variance identity.
    float deficit = (1.0 - k) * (1.0 - k);
    float base = want * curvature * curvatureScale * deficit;
    // Clamped on the BASE, not per channel, so the red-widest relationship that
    // makes skin redden rather than grey out survives the clamp. Clamping sigma
    // (dimensionless) rather than curvature (1/length) keeps this free of scene
    // scale. It is a look parameter as much as a domain guard: a nose tip or ear
    // rim runs far past it.
    base = min(base, SKIN_SIGMA_MAX / max(peakColor, 1e-3));
    return scatterColor * base;
}
