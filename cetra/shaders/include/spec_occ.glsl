// Bent-normal specular occlusion (spec 11.3 / 11.4), shared by the tonemap's
// bent mode + debug view and the split-mode composite pass so the term cannot
// drift between its consumers.

// The AO chain's bent normal is a low-order estimate -- two slices, half
// res, one blur -- so its direction is good to some degrees, not to a
// degree. This is the angular width the visibility edge is softened over,
// and it is what keeps the term from resolving that estimate more finely
// than it was ever measured.
const float VIS_EDGE_SOFTNESS = 0.25;

// How much of cone B is inside cone A, as a fraction of B's solid angle:
// the spherical-cap lens (Oat & Sander 2007). Solid angle of a half-angle-t
// cone is 2*PI*(1 - cos t), which is where the containment ratio comes from.
//
// Written branchlessly, which is not a micro-optimisation -- it is the
// correctness requirement. The geometric transition band |a-b| < d < a+b has
// half-width min(a, b), and a mirror lobe drives b to zero: the band closes
// completely and the exact function becomes a STEP at d == a. Fed a bent
// normal quantised to 8 bits, a step prints its own contour lines onto every
// smooth surface, and no amount of input precision removes them -- it only
// makes the contour thinner. So the band takes a floor, and the edge is one
// smoothstep across it.
float coneOverlap(float cosA, float cosB, float cosBetween)
{
    float a = acos(clamp(cosA, -1.0, 1.0));
    float b = acos(clamp(cosB, -1.0, 1.0));
    float d = acos(clamp(cosBetween, -1.0, 1.0));
    // Value once one cap swallows the other: all of B when A is the wider,
    // else only A's share of it. Continuous at a == b, so min() is the whole
    // case split.
    float contained = min(1.0, (1.0 - cosA) / max(1.0 - cosB, 1e-4));
    // The band is centred on max(a, b) with half-width min(a, b) -- exact
    // where the geometry has width to spare, floored where it does not.
    float half_band = max(min(a, b), VIS_EDGE_SOFTNESS);
    float mid = max(a, b);
    return contained * (1.0 - smoothstep(mid - half_band, mid + half_band, d));
}

// The directional visibility of a reflection lobe: how much of the GGX cone
// about R points somewhere the AO chain saw open sky.
float specOcclusionCone(float ao, float roughness, vec3 bentN, vec3 R)
{
    // Visibility cone: the cosine-weighted cap whose energy is the stored
    // AO, so ao = 1 opens to the full hemisphere and ao = 0 closes it.
    float cosAv = sqrt(clamp(1.0 - ao, 0.0, 1.0));
    // Reflection cone: mirror at roughness 0, near-hemispheric at 1.
    float cosAs = exp2(-3.321928 * roughness * roughness);
    float bDotR = dot(bentN, R);
    float visible = coneOverlap(cosAv, cosAs, bDotR);
    // Measured against the SAME lobe under an open hemisphere, because a
    // reflection cone always hangs partly below its own horizon and the
    // BRDF has already zeroed that half. Without the reference the term
    // charges the lobe for geometry that was never there, and even a
    // fully unoccluded surface -- open ground, a distant landscape --
    // comes back darkened.
    float open = coneOverlap(0.0, cosAs, bDotR);
    // FADE the reference out, do not switch it off at an epsilon.
    //
    // `open` is how much of the reflection lobe clears the horizon at all, so
    // the ratio below is only meaningful while there IS a lobe to speak of.
    // Guarding that with `open > 1e-4 ? ratio : 1.0` looks like a divide-by-zero
    // guard and behaves like a cliff: `visible` reaches exactly 0 well before
    // `open` does -- the two cones stop overlapping while the lobe is still
    // partly above the horizon -- so across the band where that is true the
    // branch reads 0.0, and the moment `open` slips under the epsilon it reads
    // 1.0. Fully occluded and fully open, adjacent, decided by which side of
    // 1e-4 a number landed on. On a floor around an occluder those two bands
    // are concentric, which is the ring this replaces: measured 1.000 over
    // y 1292-1310 against 0.000 over y 1316-1352 on one column of cornell_rooms.
    //
    // The width is not tuned to taste: TRUST_OPEN is where the lobe has enough
    // of itself above the horizon for the fraction to mean something, and below
    // it the term relaxes to "do not occlude" the way it always did -- just
    // continuously, so no pair of neighbouring pixels can straddle the decision.
    const float TRUST_OPEN = 0.15;
    float ratio = clamp(visible / max(open, 1e-4), 0.0, 1.0);
    return mix(1.0, ratio, smoothstep(0.0, TRUST_OPEN, open));
}

// The split-mode term for one pixel, so the composite pass and the tonemap's
// debug view call one function and cannot drift. The includer must declare
// sampler2D normalsTex (the depth.glsl contract style).
//
// Both samples arrive from the caller rather than being fetched here, which is
// what keeps the count at ONE reconstruction per pixel: these buffers are half
// res and reading either costs a four-tap depth-weighted fetch
// (ao_upsample.glsl), which the caller already performed. Fetching again here
// would double the cost to reach the same value -- or, worse, a different one,
// if the two ever stopped agreeing on how the magnification is done.
//
// There is no cone here any more. `specPair` is the reflection lobe's own
// visibility, measured against the sector bitmask inside the AO sweep where
// that mask exists (gtao_frag.glsl), and carried as the estimator's two sums so
// the denoise chain averages quantities that are linear in visibility. All this
// owes it is the divide and the guard. specOcclusionCone above is still live --
// it is what `--spec-occ bent` runs -- so the two terms remain comparable.
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
