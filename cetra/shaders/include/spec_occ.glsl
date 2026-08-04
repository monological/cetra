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
    return open > 1e-4 ? clamp(visible / open, 0.0, 1.0) : 1.0;
}

// The split-mode term for one pixel: guard, view ray, bent decode, cone --
// the whole evaluation, so the composite pass and the tonemap's debug view
// call one function and cannot drift. The includer must declare
// sampler2D aoTex / normalsTex / auxTex (the depth.glsl contract style).
float specOccSplitAt(vec2 uv, vec2 invFocal)
{
    vec4 aoSample = texture(aoTex, uv);
    vec4 nrm = texture(normalsTex, uv);
    // Sky/hair (zero normal) and the shadow-catcher floor: no trustworthy
    // reflection direction, so the plain AO answer serves. The catcher test
    // is the marker's SIGN, matching what the catcher writes and the SSR
    // march reads -- its magnitude is the edge falloff, so any threshold
    // would hand part of the plane's outer ring to the cone term.
    if (dot(nrm.xyz, nrm.xyz) < 0.01 || nrm.a < 0.0)
        return aoSample.r;
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 V = normalize(vec3(-ndc * invFocal, 1.0));
    vec3 N = normalize(nrm.xyz);
    vec3 bentN = normalize(aoSample.gba * 2.0 - 1.0);
    return specOcclusionCone(aoSample.r, texture(auxTex, uv).w, bentN, reflect(-V, N));
}
