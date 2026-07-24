// Linearly Transformed Cosines: rectangular area lights (Heitz, Dupuy, Hill,
// Neubelt, SIGGRAPH 2016). Spec 9.2.
//
// The two lookup tables are fitted offline and vendored (tools/ltc_data.js ->
// cetra/src/ltc_lut.h). ltcMatTex holds the inverse-M transform that maps the
// GGX lobe onto a clamped cosine; ltcAmpTex holds the magnitude/Fresnel terms
// in .xy and, at its OWN uv, the horizon-clipped-sphere form factor in .w.
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

uniform sampler2D ltcMatTex; // inverse-M fit          (TEXUNIT_LTC_MAT)
uniform sampler2D ltcAmpTex; // magnitude/Fresnel + .w (TEXUNIT_LTC_AMP)

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
    vec4 t = textureLod(ltcMatTex, coords, 0.0);
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

// Integrate a quad against the cosine lobe that Minv maps the BRDF onto.
// Clipless formulation: rather than clipping the polygon to the horizon, sum
// the four edge vectors and look the result up in the sphere table, which
// carries the clip. Pass mat3(1.0) for Lambert diffuse.
float ltcEvaluate(vec3 N, vec3 V, vec3 P, mat3 Minv, vec3 p0, vec3 p1, vec3 p2, vec3 p3) {
    // Orthonormal basis around N, aligned so V lies in the T1-N plane
    vec3 T1 = normalize(V - N * dot(V, N));
    vec3 T2 = cross(N, T1);

    mat3 M = Minv * transpose(mat3(T1, T2, N));

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
    // own uv, NOT the roughness/NdotV coordinate above
    float z = vsum.z / len;
    vec2 uv = vec2(z * 0.5 + 0.5, len) * LTC_LUT_SCALE + LTC_LUT_BIAS;
    return len * textureLod(ltcAmpTex, uv, 0.0).w;
}

// Panel corners from center + orientation. `up` spans the height axis and
// `dir` is the emitting normal (both arrive orthonormal from the CPU), so
// width follows from cross(up, dir) and the winding below puts the polygon
// normal along +dir -- the side the panel lights.
void ltcPanelCorners(vec3 center, vec3 dir, vec3 up, vec2 halfSize, out vec3 p0, out vec3 p1,
                     out vec3 p2, out vec3 p3) {
    vec3 right = cross(up, dir) * halfSize.x;
    vec3 top = up * halfSize.y;
    p0 = center - right - top;
    p1 = center + right - top;
    p2 = center + right + top;
    p3 = center - right + top;
}
