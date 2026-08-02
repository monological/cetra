#version 330 core
in vec3 WorldPos;
out vec4 FragColor;

// Charlie-kernel environment prefilter (spec 10.7.1): the sheen sibling of
// ibl_prefilter_frag, same contract (N = V = R convolution, one mip per
// roughness step, solid-angle mip selection on the source). The kernel is
// the SHARED distributionCharlie -- the filter is the shading lobe by
// construction -- which blurs far wider than GGX at equal roughness; that
// width is the whole reason this chain exists.

uniform samplerCube environmentMap;
uniform float roughness;
uniform float resolution; // Face size of environmentMap mip 0

#include "sampling.glsl"
#include "sheen.glsl"

// Importance-sample the Charlie NDF (inverse-CDF from the reference
// renderer): sin(theta_h) = u^(alpha / (2*alpha + 1)). Tangent-basis
// construction mirrors ImportanceSampleGGX.
vec3 ImportanceSampleCharlie(vec2 Xi, vec3 N, float r)
{
    float alpha = max(r * r, 1e-4);
    float phi = 2.0 * PI * Xi.x;
    float sinTheta = pow(Xi.y, alpha / (2.0 * alpha + 1.0));
    float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.0));

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

void main()
{
    vec3 N = normalize(WorldPos);
    vec3 R = N;
    vec3 V = R;

    float r = max(roughness, 0.01);

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefilteredColor = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H = ImportanceSampleCharlie(Xi, N, r);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float D = distributionCharlie(N, H, r);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += textureLod(environmentMap, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    prefilteredColor = prefilteredColor / max(totalWeight, 1e-4);

    FragColor = vec4(prefilteredColor, 1.0);
}
