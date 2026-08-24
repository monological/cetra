// The split-mode specular-occlusion term, shared by the composite pass and the
// tonemap's debug view so the applied value and the inspected one cannot drift.
//
// This file used to hold the term itself: a cone about the AO chain's bent
// normal intersected with a cone about the reflection vector (Klehm et al.
// 2011, via specs 11.3 / 11.4). That arithmetic is gone as of spec 11.77. The
// term is now measured against the sector bitmask inside the AO sweep, where
// the directional information still exists, and arrives here as two sums.

// The includer must declare sampler2D normalsTex (the depth.glsl contract
// style).
//
// Both samples arrive from the caller rather than being fetched here, which is
// what keeps the count at ONE reconstruction per pixel: these buffers are half
// res and reading either costs a four-tap depth-weighted fetch
// (ao_upsample.glsl), which the caller already performed. Fetching again here
// would double the cost to reach the same value -- or, worse, a different one,
// if the two ever stopped agreeing on how the magnification is done.
//
// `specPair` is the reflection lobe's own visibility, measured against the
// sector bitmask inside the AO sweep where that mask exists (gtao_frag.glsl),
// and carried as the estimator's two sums so the denoise chain averages
// quantities that are linear in visibility. All this owes it is the divide and
// the guard.
float specOccSplitAt(vec2 uv, vec4 aoSample, vec2 specPair)
{
    // Sky/hair (zero normal) and the shadow-catcher floor: no trustworthy
    // reflection direction, so the plain AO answer serves. The catcher test
    // is the marker's SIGN, matching what the catcher writes and the SSR
    // march reads -- its magnitude is the edge falloff, so any threshold
    // would hand part of the plane's outer ring to the specular term.
    vec4 nrm = texture(normalsTex, uv);
    if (dot(nrm.xyz, nrm.xyz) < 0.01 || nrm.a < 0.0)
        return aoSample.r;
    // No lobe above the horizon anywhere in the sweep -- including every pixel
    // whose sums came from the early-outs' (0,0) neutral. Nothing to occlude,
    // and the BRDF has already zeroed what sits below.
    return specPair.y > 1e-5 ? clamp(specPair.x / specPair.y, 0.0, 1.0) : 1.0;
}
