// KHR_materials_sheen Charlie lobe, shared between the forward shading pass
// and the BRDF-LUT bake so the directional albedo the LUT integrates is the
// exact lobe the shading pass evaluates.

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

// Ashikhmin visibility term, paired with the Charlie NDF -- the KHR spec's
// sanctioned cheaper alternative to the Charlie lambda form. The E channel
// in the BRDF LUT integrates THIS pair; switching either half means
// re-deriving the other.
float visibilityAshikhmin(float NdotL, float NdotV) {
    return clamp(1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV)), 0.0, 1.0);
}
