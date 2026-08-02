#version 330 core
in vec2 TexCoords;
out vec3 FragColor;

#include "sampling.glsl"
#include "sheen.glsl"

// NOTE the k below is the IBL form, k = a^2/2. The prefilter shader uses the
// direct-lighting form for the same-named function; that divergence is
// intentional and is why the geometry terms are NOT in the shared chunk.
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

vec2 IntegrateBRDF(float NdotV, float roughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0)
        {
            float G = GeometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    return vec2(A, B);
}

// Directional albedo of the sheen lobe: E(NdotV, sheenRoughness) =
// integral of Dcharlie * Vashikhmin * NdotL over the hemisphere -- the
// sheen albedo-scaling factor and the integrated-BRDF half of the sheen
// split-sum. Uniform-hemisphere sampling rather than importance sampling:
// the Charlie lobe is a wide grazing ring, so uniform L converges fine and
// there is no half-vector Jacobian to get wrong. y is the PERCEPTUAL
// roughness -- squaring happens inside distributionCharlie, so the bake and
// the runtime fetch share one coordinate. Rows below SHEEN_MIN_ROUGHNESS
// integrate a near-delta ring too sparsely to trust; shading clamps to that
// floor, so they are never fetched.
float IntegrateCharlieE(float NdotV, float sheenRoughness)
{
    vec3 V;
    V.x = sqrt(1.0 - NdotV * NdotV);
    V.y = 0.0;
    V.z = NdotV;

    vec3 N = vec3(0.0, 0.0, 1.0);

    float E = 0.0;
    const uint SAMPLE_COUNT = 2048u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        // Uniform hemisphere: cos(theta) = Xi.x gives constant pdf 1/(2*PI)
        float cosT = Xi.x;
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float phi = 2.0 * PI * Xi.y;
        vec3 L = vec3(sinT * cos(phi), sinT * sin(phi), cosT);
        vec3 H = normalize(V + L);

        float D = distributionCharlie(N, H, sheenRoughness);
        float Vis = visibilitySheen(cosT, NdotV, sheenRoughness);
        E += D * Vis * cosT;
    }
    // f/pdf mean: multiply by 2*PI, divide by the sample count.
    return E * 2.0 * PI / float(SAMPLE_COUNT);
}

void main()
{
    vec2 integratedBRDF = IntegrateBRDF(TexCoords.x, TexCoords.y);
    float charlieE = IntegrateCharlieE(TexCoords.x, TexCoords.y);
    FragColor = vec3(integratedBRDF, charlieE);
}
