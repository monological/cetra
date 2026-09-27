// The Schlick Fresnel forms, shared by every surface that has an interface.
//
// One place because the roughness variant in particular is easy to write two ways: the
// `max(1 - roughness, F0)` cap is what keeps a rough metal's grazing reflection from
// exceeding its own base reflectance, and a copy that dropped it would look almost right.

// Fresnel-Schlick approximation.
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// The exact unpolarised Fresnel reflectance of a dielectric interface, light arriving from the
// side of index 1 at cosine `cosi` into index `n`; 1 past total internal reflection.
//
// Schlick is fitted at normal incidence and drifts toward grazing, which is where a sea
// reflects most of what it reflects and which Schlick puts too low for water. A surface that
// is one interface all the way to its horizon takes the exact form.
float fresnelDielectric(float cosi, float n) {
    cosi = clamp(cosi, 0.0, 1.0);
    float sint2 = (1.0 - cosi * cosi) / (n * n);
    if (sint2 >= 1.0)
        return 1.0;
    float cost = sqrt(1.0 - sint2);
    float rs = (cosi - n * cost) / (cosi + n * cost);
    float rp = (n * cosi - cost) / (n * cosi + cost);
    return 0.5 * (rs * rs + rp * rp);
}

// Fresnel-Schlick with roughness, for the split-sum IBL lookup.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
