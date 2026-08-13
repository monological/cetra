// The Schlick Fresnel forms, shared by every surface that has an interface.
//
// One place because the roughness variant in particular is easy to write two ways: the
// `max(1 - roughness, F0)` cap is what keeps a rough metal's grazing reflection from
// exceeding its own base reflectance, and a copy that dropped it would look almost right.

// Fresnel-Schlick approximation.
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness, for the split-sum IBL lookup.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
