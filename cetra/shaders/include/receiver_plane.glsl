// Receiver-plane depth bias (Isidoro, GDC 2006): the pieces shared by every
// shadow lookup, projection-independent. Each consumer builds its own
// shadow-space derivatives (perspective needs divides and w guards,
// punctual_shadow.glsl; ortho is one linear transform, the cascade path) and
// its own tap loops; what lives here is the policy those loops must agree on.
//
// One depth-buffer ULP: both shadow arrays are DEPTH_COMPONENT24.
#define SHADOW_DEPTH24_ULP (1.0 / 16777216.0)
// Constant floor under the plane bias, in ULPs. The plane is exact for a
// planar receiver, so what is left for a constant to cover is what a plane
// cannot describe: quantization, and curvature within one texel.
#define SHADOW_PLANE_BIAS_FLOOR (4.0 * SHADOW_DEPTH24_ULP)
// How far the plane may be extrapolated toward the light, as a fraction of
// the depth range per texel. A silhouette is where the receiver plane stops
// being the surface the kernel is sampling, and an unbounded extrapolation
// there turns the bias into a light leak that widens with the slope.
#define SHADOW_MAX_PLANE_BIAS 0.01

// How the receiver's depth changes per unit of shadow UV, from its
// shadow-space derivatives along the two screen axes. A degenerate 2x2 means
// the receiver projects to a line in the map -- exactly edge-on -- where
// there is no plane to extrapolate along.
vec2 receiverPlaneGradient(vec3 px, vec3 py) {
    float det = px.x * py.y - px.y * py.x;
    if (abs(det) < 1e-12)
        return vec2(0.0);
    return vec2(py.y * px.z - px.y * py.z, px.x * py.z - py.x * px.z) / det;
}

// Where the receiver's own plane sits at a tap offset, clamped ONE-SIDED:
// the plane may only ever make the test more lenient. Its whole job is the
// nearer direction -- matching the receiver's own surface up-slope so
// grazing acne dies. A deeper prediction only ever makes the test stricter
// than a flat compare, and at a concave junction that manufactures
// occlusion: the plane extrapolates INTO the adjacent surface, which
// truncates it and stands epsilon NEARER, so taps crossing the junction read
// "occluded" at coplanar scale. Measured on cornell_leak's wall base: every
// failing tap failed with a sub-1e-4 depth gap and none held a genuinely
// different surface -- the dark wedge was entirely this extrapolation.
float receiverPlaneBias(vec2 duv_dz, vec2 off) {
    return clamp(dot(duv_dz, off), -SHADOW_MAX_PLANE_BIAS, 0.0);
}
