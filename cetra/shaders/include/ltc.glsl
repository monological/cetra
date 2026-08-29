// Linearly Transformed Cosines: rectangular area lights (Heitz, Dupuy, Hill,
// Neubelt, SIGGRAPH 2016). Spec 9.2.
//
// The two lookup tables are fitted offline and vendored (tools/ltc_data.js ->
// cetra/src/ltc_lut.h), packed as two layers of one array texture. Layer 0
// holds the inverse-M transform that maps the GGX lobe onto a clamped
// cosine; layer 1 holds the magnitude/Fresnel terms in .xy and, at its OWN
// uv, the horizon-clipped-sphere form factor in .w.
//
// NORMALIZATION CONTRACT: the .w table already folds in 1/(2*pi) along with
// the horizon clip, and the form factor it returns is the cosine-weighted
// integral divided by pi. So the caller applies NO 2*pi and NO 1/pi -- adding
// either (a tempting "fix" when the result looks dim) double-corrects and is
// wrong. Diffuse is albedo * formFactor; specular is formFactor scaled by the
// amplitude pair.
//
// Every fetch uses textureLod(..., 0.0): these run inside the clustered light
// loop, which is non-uniform control flow where implicit-derivative sampling
// is undefined. The tables are mip-less anyway.
//
// COMPILED OUT WHOLE on a variant without area lights (spec 11.95), declaration
// included -- which is the point, because a sampler survives having every read
// folded away and goes on spending one of the sixteen units. The file gates
// itself rather than being gated at its include site: it is the only place that
// knows ltcTex is what it is, and a guard around the `#include` would take the
// functions pbr_frag calls along with it.
//
// pbr_features.glsl defaults the mask to the full set, so a shader that never
// heard of variants includes this and keeps everything.
#include "pbr_features.glsl"
#if CETRA_HAS(PBR_FEAT_AREA)

uniform sampler2DArray ltcTex; // layer 0 inverse-M, layer 1 magnitude/Fresnel + .w (TEXUNIT_LTC)

const float LTC_LAYER_MAT = 0.0;
const float LTC_LAYER_AMP = 1.0;

// Lowest roughness the quad integral stays numerically sound at; see the
// caller in pbr_frag.glsl for the derivation and its limits.
const float LTC_MIN_ROUGHNESS = 0.12;

const float LTC_LUT_SIZE = 64.0;
const float LTC_LUT_SCALE = (LTC_LUT_SIZE - 1.0) / LTC_LUT_SIZE;
const float LTC_LUT_BIAS = 0.5 / LTC_LUT_SIZE;

// Table coordinate for a surface: roughness across, view angle down. The
// scale/bias pair keeps the fetch inside the texel centers.
vec2 ltcCoords(float roughness, float NdotV) {
    vec2 uv = vec2(roughness, sqrt(1.0 - clamp(NdotV, 0.0, 1.0)));
    return uv * LTC_LUT_SCALE + LTC_LUT_BIAS;
}

// Rebuild the inverse-M matrix from the packed fit. The fit normalizes
// m[1][1] to 1, so only the x/z block is stored (as a, b, c, d). Columns.
mat3 ltcMatrix(vec2 coords) {
    vec4 t = textureLod(ltcTex, vec3(coords, LTC_LAYER_MAT), 0.0);
    return mat3(vec3(t.x, 0.0, t.y), vec3(0.0, 1.0, 0.0), vec3(t.z, 0.0, t.w));
}

// Hill's rational fit of theta/sin(theta), in vector form: the analytic
// integral along one polygon edge, scaled onto the edge's rotation axis.
vec3 ltcIntegrateEdgeVec(vec3 v1, vec3 v2) {
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float theta_sintheta = x > 0.0 ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * theta_sintheta;
}

// Integrate a quad against the cosine lobe. Clipless formulation: rather than
// clipping the polygon to the horizon, sum the four edge vectors and look the
// result up in the sphere table, which carries the clip.
//
// `M` maps a world-space corner offset into the lobe's own space: the plain
// (T1,T2,N) shading basis for diffuse, that basis pre-multiplied by Minv for
// specular, which is what warps the cosine into the GGX lobe. Folding Minv
// into the basis once beats transforming four corners twice.
float _ltcIntegrate(mat3 M, vec3 P, vec3 p0, vec3 p1, vec3 p2, vec3 p3) {
    // Which face of the quad this fragment sees. The edge-vector sum is a
    // SIGNED area: its tilt comes out negated for a fragment looking at the
    // back of the winding, and the corners are wound along +dir -- the side the
    // panel lights -- so that is every fragment the plane test admits.
    //
    // Without this the horizon clip floors the form factor at zero for a
    // surface facing the panel and passes the grazing and back-facing ones
    // instead, i.e. the whole response is inverted. It reads as a falloff
    // problem rather than a sign one, which is how it survived: a panel
    // overhead lights the BOTTOM of a sphere, which still looks like lighting.
    //
    // Kept as the reference's per-fragment test rather than folded into the
    // winding. The two are equivalent only while the plane test above admits
    // exactly the +dir side; the test is what makes that a property of this
    // function instead of an invariant split across two of them.
    bool behind = dot(p0 - P, cross(p1 - p0, p3 - p0)) < 0.0;

    vec3 L0 = normalize(M * (p0 - P));
    vec3 L1 = normalize(M * (p1 - P));
    vec3 L2 = normalize(M * (p2 - P));
    vec3 L3 = normalize(M * (p3 - P));

    vec3 vsum = vec3(0.0);
    vsum += ltcIntegrateEdgeVec(L0, L1);
    vsum += ltcIntegrateEdgeVec(L1, L2);
    vsum += ltcIntegrateEdgeVec(L2, L3);
    vsum += ltcIntegrateEdgeVec(L3, L0);

    float len = length(vsum);
    if (len < 1e-7)
        return 0.0;

    // Horizon-clipped sphere: indexed by the sum's tilt and magnitude at its
    // own uv, NOT the roughness/NdotV coordinate the tables were fetched with
    float z = vsum.z / len;
    if (behind)
        z = -z;
    vec2 uv = vec2(z * 0.5 + 0.5, len) * LTC_LUT_SCALE + LTC_LUT_BIAS;
    return len * textureLod(ltcTex, vec3(uv, LTC_LAYER_AMP), 0.0).w;
}

// Both form factors for one rectangular panel: .x diffuse (Lambert), .y
// specular (the GGX lobe Minv maps onto the cosine). Returns zero for a
// fragment behind the panel -- v1 panels are single-sided, and this exact
// plane test pins that convention independently of the corner winding.
//
// `up` spans the panel's height axis and `dir` is its emitting normal (both
// arrive orthonormal from the CPU), so width is cross(up, dir) and the winding
// puts the polygon normal along +dir: the side the panel lights.
//
// The shading basis and the corner vectors are shared by both lobes -- only
// the matrix applied to them differs -- so this computes them once rather than
// integrating the quad twice from scratch.
vec2 ltcPanel(vec3 N, vec3 V, vec3 P, mat3 Minv, vec3 center, vec3 dir, vec3 up, vec2 halfSize) {
    if (dot(P - center, dir) <= 0.0)
        return vec2(0.0);

    vec3 right = cross(up, dir) * halfSize.x;
    vec3 top = up * halfSize.y;
    vec3 p0 = center - right - top;
    vec3 p1 = center + right - top;
    vec3 p2 = center + right + top;
    vec3 p3 = center - right + top;

    // Orthonormal basis around N, aligned so V lies in the T1-N plane.
    //
    // V PARALLEL TO N leaves nothing to normalize. That is not a corner case --
    // it is the centre of every flat surface facing the camera, the back wall of
    // a room being the obvious one -- and normalize(0) is a NaN that propagates
    // through all four corner vectors into the form-factor fetch, where it comes
    // back as a surface lit by a panel that is behind it. Any tangent will do
    // when it happens: the cosine lobe is rotationally symmetric about N, and at
    // normal incidence so is the GGX lobe Minv maps onto it.
    vec3 tangent = V - N * dot(V, N);
    float tangentLen = length(tangent);
    vec3 T1 = tangentLen > 1e-4
                  ? tangent / tangentLen
                  : normalize(cross(abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N));
    vec3 T2 = cross(N, T1);
    mat3 basis = transpose(mat3(T1, T2, N));

    // Diffuse is the cosine lobe itself, so it takes the basis unwarped
    return vec2(_ltcIntegrate(basis, P, p0, p1, p2, p3),
                _ltcIntegrate(Minv * basis, P, p0, p1, p2, p3));
}

#endif // CETRA_HAS(PBR_FEAT_AREA)
