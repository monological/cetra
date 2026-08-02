// KHR_materials_sheen Charlie lobe, shared between the forward shading pass,
// the BRDF-LUT bake, and the Charlie environment prefilter so every consumer
// integrates or evaluates the exact same lobe.

// Floor on the PERCEPTUAL sheen roughness, shared by the shading clamp and
// the bake's trusted domain: below it the squared-alpha lobe is a near-delta
// ring that both aliases in the forward pass and is sampled too sparsely by
// the E integration to trust.
const float SHEEN_MIN_ROUGHNESS = 0.07;

// Charlie sheen distribution (Estevez-Kulla, "Production Friendly Microfacet
// Sheen"): an inverted-Gaussian NDF giving cloth its retroreflective grazing
// rim. alpha = sheenRoughness^2, the KHR spec's perceptually-linear mapping.
float distributionCharlie(vec3 N, vec3 H, float sheenRoughness) {
    float alphaG = max(sheenRoughness * sheenRoughness, 1e-4);
    float invAlpha = 1.0 / alphaG;
    float NdotH = max(dot(N, H), 0.0);
    float sin2h = max(1.0 - NdotH * NdotH, 0.0078125); // fp16-safe floor
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * 3.14159265359);
}

// Numeric fit for the Charlie lambda term (Conty-Kulla via the KHR spec's
// implementation notes; constants verbatim from the reference renderer).
float lambdaSheenNumericHelper(float x, float alphaG) {
    float oneMinusAlphaSq = (1.0 - alphaG) * (1.0 - alphaG);
    float a = mix(21.5473, 25.3245, oneMinusAlphaSq);
    float b = mix(3.82987, 3.32435, oneMinusAlphaSq);
    float c = mix(0.19823, 0.16801, oneMinusAlphaSq);
    float d = mix(-1.97760, -1.27393, oneMinusAlphaSq);
    float e = mix(-4.32054, -4.85967, oneMinusAlphaSq);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}

float lambdaSheen(float cosTheta, float alphaG) {
    return abs(cosTheta) < 0.5
               ? exp(lambdaSheenNumericHelper(cosTheta, alphaG))
               : exp(2.0 * lambdaSheenNumericHelper(0.5, alphaG) -
                     lambdaSheenNumericHelper(1.0 - cosTheta, alphaG));
}

// Charlie-lambda visibility, the KHR spec's energy-conserving primary form
// (it replaced the sanctioned-but-lossy Ashikhmin fallback in 10.7.1). The
// E channel in the BRDF LUT integrates THIS pair with distributionCharlie;
// switching either half means re-deriving the other. alpha matches D's
// squared mapping so both halves see one lobe width.
float visibilitySheen(float NdotL, float NdotV, float sheenRoughness) {
    float alphaG = max(sheenRoughness * sheenRoughness, 1e-4);
    return clamp(1.0 / ((1.0 + lambdaSheen(NdotV, alphaG) + lambdaSheen(NdotL, alphaG)) *
                        (4.0 * NdotV * NdotL)),
                 0.0, 1.0);
}
