// Quasi-random GGX importance sampling, shared by the two halves of the
// split-sum IBL approximation: ibl_prefilter_frag (the prefiltered radiance
// mips) and ibl_brdf_frag (the BRDF LUT).
//
// These MUST sample the same distribution. Split-sum is only valid because the
// LUT's integral and the prefilter's integral assume the same GGX lobe -- if
// they drift apart every specular highlight in the engine is energetically
// wrong, and the error is smooth and plausible-looking rather than obviously
// broken, so it would not be caught by eye.
//
// Not shared, deliberately: DistributionGGX and GeometrySmith. Those two DO
// differ between the files, and correctly so -- the prefilter uses the
// direct-lighting k while the BRDF LUT uses the IBL k. Folding them together
// would be the one change here that actually breaks the maths.

const float PI = 3.14159265359;

// Van der Corput radical inverse: bit-reverse i and scale to [0,1).
float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Hammersley point set: the i-th of N low-discrepancy samples on the unit square.
vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

// Map a unit-square sample to a half-vector distributed by the GGX NDF around
// N, for the given roughness.
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}
