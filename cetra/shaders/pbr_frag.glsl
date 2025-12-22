#version 330 core
in vec3 Normal;
in vec3 WorldPos;
in vec3 ViewPos;
in vec3 FragPos;
in float ClipDepth;
in float FragDepth;
in vec2 TexCoords;
in mat3 TBN;
out vec4 FragColor;

#define MAX_LIGHTS 70

struct Light {
    int type;
    vec3 position;
    vec3 direction;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
    vec2 size;
};

uniform Light lights[MAX_LIGHTS];
uniform int numLights;

uniform mat4 view;
uniform mat4 model;
uniform mat4 projection;

uniform int renderMode;
uniform float nearClip;
uniform float farClip;

uniform vec3 albedo;
uniform float metallic;
uniform float roughness;
uniform float ao;
uniform float materialOpacity;
uniform float ior;
uniform float filmThickness;
uniform vec3 camPos;
uniform float time;

uniform sampler2D albedoTex;
uniform sampler2D normalTex;
uniform sampler2D roughnessTex;
uniform sampler2D metalnessTex;
uniform sampler2D aoTex;
uniform sampler2D emissiveTex;
uniform sampler2D heightTex;
uniform sampler2D opacityTex;
uniform sampler2D sheenTex;
uniform sampler2D reflectanceTex;
uniform sampler2D microsurfaceTex;
uniform sampler2D anisotropyTex;
uniform sampler2D subsurfaceTex;

uniform int albedoTexExists;
uniform int normalTexExists;
uniform int roughnessTexExists;
uniform int metalnessTexExists;
uniform int aoTexExists;
uniform int emissiveTexExists;
uniform int heightTexExists;
uniform int opacityTexExists;
uniform int sheenTexExists;
uniform int reflectanceTexExists;
uniform int microsurfaceTexExists;
uniform int anisotropyTexExists;
uniform int subsurfaceTexExists;

// Shadow mapping uniforms
#define MAX_SHADOW_LIGHTS 3
uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS];
uniform int shadowLightIndex[MAX_SHADOW_LIGHTS];
uniform int numShadowLights;
uniform float shadowBias;
uniform vec2 shadowTexelSize;

// IBL (Image-Based Lighting) uniforms
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;

const float PI = 3.14159265359;

// Color space conversions
vec3 sRGBToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 linearToSRGB(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Thin-film interference for iridescent coatings (pilot visor effect)
// thickness: film thickness in nanometers (200-600nm typical)
// cosTheta: dot(N, V) - viewing angle
// filmIOR: refractive index of the thin film coating (~1.5 for most coatings)
vec3 thinFilmInterference(float thickness, float cosTheta, float filmIOR) {
    // Wavelengths in nanometers for RGB
    const vec3 wavelengths = vec3(650.0, 550.0, 450.0); // R, G, B

    // Refracted angle in the film (Snell's law, assuming air n=1.0)
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float sinThetaFilm = sinTheta / filmIOR;
    float cosThetaFilm = sqrt(1.0 - sinThetaFilm * sinThetaFilm);

    // Optical path difference (2 * n * d * cos(theta_film))
    float opd = 2.0 * filmIOR * thickness * cosThetaFilm;

    // Phase shift for each wavelength
    vec3 phase = 2.0 * PI * opd / wavelengths;

    // Interference: (1 + cos(phase)) / 2 gives 0-1 range
    // Add phase shift of PI for reflection from denser medium
    vec3 interference = 0.5 + 0.5 * cos(phase + PI);

    // Boost saturation for more vivid colors
    vec3 color = interference;
    float avg = (color.r + color.g + color.b) / 3.0;
    color = mix(vec3(avg), color, 1.5); // Increase saturation

    return clamp(color, 0.0, 1.0);
}

// GGX/Trowbridge-Reitz Normal Distribution Function
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// Anisotropic GGX distribution (for brushed metal, hair, etc.)
float distributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B, float roughness, float anisotropy) {
    float at = max(roughness * (1.0 + anisotropy), 0.001);
    float ab = max(roughness * (1.0 - anisotropy), 0.001);

    float ToH = dot(T, H);
    float BoH = dot(B, H);
    float NoH = max(dot(N, H), 0.0);

    float a2 = at * ab;
    vec3 v = vec3(ab * ToH, at * BoH, a2 * NoH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;

    return a2 * w2 * w2 / PI;
}

// Subsurface scattering approximation using wrap lighting
vec3 subsurfaceScattering(vec3 N, vec3 L, vec3 V, vec3 albedo, float thickness, vec3 lightColor) {
    // Wrap lighting for diffuse transmission
    float wrap = 0.5;
    float NdotL = dot(N, L);
    float wrapDiffuse = max(0.0, (NdotL + wrap) / (1.0 + wrap));

    // Back-lighting transmission
    float transmittance = exp(-thickness * 2.0);
    vec3 backLight = albedo * lightColor * transmittance * max(0.0, -NdotL);

    return backLight * 0.5;
}

// Smith's Schlick-GGX geometry function for a single direction
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

// Smith's geometry function combining view and light directions
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Attenuation for point/spot lights
float calculateAttenuation(float distance, float constant, float linear, float quadratic) {
    return 1.0 / (constant + linear * distance + quadratic * (distance * distance));
}

// PCF soft shadow calculation
float calculateShadow(int shadowIndex, vec3 worldPos, float NdotL) {
    vec4 fragPosLightSpace = lightSpaceMatrix[shadowIndex] * vec4(worldPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 1.0;
    }

    float bias = max(shadowBias * (1.0 - NdotL), shadowBias * 0.1);
    float currentDepth = projCoords.z;

    // PCF 3x3 kernel
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            float pcfDepth = texture(shadowMaps, vec3(projCoords.xy + offset, float(shadowIndex))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return 1.0 - (shadow / 9.0);
}

// Find shadow slot for a given light index (-1 if not shadow-casting)
int getShadowSlot(int lightIndex) {
    for (int i = 0; i < numShadowLights && i < MAX_SHADOW_LIGHTS; i++) {
        if (shadowLightIndex[i] == lightIndex) {
            return i;
        }
    }
    return -1;
}

void main() {
    // Early-out for simple render modes that don't need texture sampling
    if (renderMode == 5) {
        // Flat Color - no textures needed
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
        return;
    }
    if (renderMode == 1) {
        // Normals Visualization - no textures needed
        vec3 color = normalize(Normal) * 0.5 + 0.5;
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 2) {
        // World Position Visualization - no textures needed
        vec3 color = fract(WorldPos * 0.01);
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 3) {
        // Texture Coordinates Visualization - no textures needed
        FragColor = vec4(TexCoords, 0.0, 1.0);
        return;
    }
    if (renderMode == 4) {
        // Tangent Space Visualization - no textures needed
        vec3 tangent = normalize(TBN[0]) * 0.5 + 0.5;
        FragColor = vec4(tangent, 1.0);
        return;
    }
    if (renderMode == 6) {
        // Albedo Only - only sample albedo texture
        vec3 albedoMap = albedo;
        if (albedoTexExists > 0) {
            albedoMap = texture(albedoTex, TexCoords).rgb;
            albedoMap = sRGBToLinear(albedoMap);
        }
        vec3 color = linearToSRGB(albedoMap);
        FragColor = vec4(color, materialOpacity);
        return;
    }

    // Sample material properties from textures or use uniforms
    vec3 albedoMap = albedo;
    if (albedoTexExists > 0) {
        albedoMap = texture(albedoTex, TexCoords).rgb;
        albedoMap = sRGBToLinear(albedoMap);
    }

    vec3 N;
    if (normalTexExists > 0) {
        N = texture(normalTex, TexCoords).rgb;
        N = N * 2.0 - 1.0;
        N = normalize(TBN * N);
    } else {
        N = normalize(Normal);
    }

    float roughnessMap = roughness;
    if (roughnessTexExists > 0) {
        roughnessMap = texture(roughnessTex, TexCoords).r;
    }
    // Clamp roughness to avoid division issues
    roughnessMap = clamp(roughnessMap, 0.04, 1.0);

    float metallicMap = metallic;
    if (metalnessTexExists > 0) {
        metallicMap = texture(metalnessTex, TexCoords).r;
    }

    float aoMap = ao;
    if (aoTexExists > 0) {
        aoMap = texture(aoTex, TexCoords).r;
    }

    vec3 emissiveMap = vec3(0.0);
    if (emissiveTexExists > 0) {
        emissiveMap = sRGBToLinear(texture(emissiveTex, TexCoords).rgb);
    }

    float opacity = materialOpacity;
    if (opacityTexExists > 0) {
        opacity = texture(opacityTex, TexCoords).r * materialOpacity;
    }

    // Microsurface detail - modulates roughness for fine surface detail
    if (microsurfaceTexExists > 0) {
        float detail = texture(microsurfaceTex, TexCoords).r;
        roughnessMap = clamp(roughnessMap * (0.5 + detail), 0.04, 1.0);
    }

    // Anisotropy - for brushed metal, hair effects
    float anisotropyMap = 0.0;
    if (anisotropyTexExists > 0) {
        anisotropyMap = texture(anisotropyTex, TexCoords).r;
    }

    // Subsurface scattering thickness map
    float sssThickness = 1.0;
    if (subsurfaceTexExists > 0) {
        sssThickness = texture(subsurfaceTex, TexCoords).r;
    }

    // Calculate view direction (must use WorldPos, not FragPos which is clip space)
    vec3 V = normalize(camPos - WorldPos);

    // Render modes that need texture data
    if (renderMode == 7) {
        // Simple Diffuse Lighting
        vec3 Lo = vec3(0.0);
        for (int i = 0; i < numLights; i++) {
            vec3 L;
            float attenuation;
            if (lights[i].type == 0) {
                L = normalize(-lights[i].direction);
                attenuation = 1.0;
            } else {
                L = normalize(lights[i].position - WorldPos);
                float distance = length(lights[i].position - WorldPos);
                attenuation = calculateAttenuation(distance, lights[i].constant,
                                                   lights[i].linear, lights[i].quadratic);
            }
            float NdotL = max(dot(N, L), 0.0);
            Lo += albedoMap * lights[i].color * lights[i].intensity * attenuation * NdotL;
        }
        vec3 color = Lo + vec3(0.03) * albedoMap;
        color = color / (color + vec3(1.0));
        color = linearToSRGB(color);
        FragColor = vec4(color, opacity);
        return;
    }
    if (renderMode == 8) {
        // Metallic and Roughness Visualization
        FragColor = vec4(metallicMap, roughnessMap, 0.0, 1.0);
        return;
    }

    // renderMode == 0: Full PBR

    // Calculate F0 (surface reflection at zero incidence) from IOR
    // F0 = ((ior - 1) / (ior + 1))^2
    // For plastic/glass (ior=1.5): F0 = 0.04
    float iorF0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F0 = vec3(iorF0);
    F0 = mix(F0, albedoMap, metallicMap);

    // Accumulate lighting from all lights
    vec3 Lo = vec3(0.0);

    // Get tangent and bitangent for anisotropy
    vec3 T = normalize(TBN[0]);
    vec3 B = normalize(TBN[1]);

    for (int i = 0; i < numLights; i++) {
        // Calculate per-light radiance
        vec3 L;
        float attenuation;

        if (lights[i].type == 0) {
            // LIGHT_DIRECTIONAL: use direction, no attenuation
            L = normalize(-lights[i].direction);
            attenuation = 1.0;
        } else {
            // Point/Spot lights: use position-based calculation
            L = normalize(lights[i].position - WorldPos);
            float distance = length(lights[i].position - WorldPos);
            attenuation = calculateAttenuation(distance, lights[i].constant,
                                               lights[i].linear, lights[i].quadratic);
        }

        vec3 H = normalize(V + L);
        vec3 radiance = lights[i].color * lights[i].intensity * attenuation;

        // Cook-Torrance BRDF with optional anisotropy
        float NDF;
        if (anisotropyTexExists > 0 && anisotropyMap > 0.01) {
            NDF = distributionGGXAnisotropic(N, H, T, B, roughnessMap, anisotropyMap);
        } else {
            NDF = distributionGGX(N, H, roughnessMap);
        }
        float G = geometrySmith(N, V, L, roughnessMap);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        // Apply thin-film interference for iridescent coatings (pilot visor style)
        if (filmThickness > 0.0) {
            float NdotV = max(dot(N, V), 0.0);
            vec3 iridescence = thinFilmInterference(filmThickness, NdotV, 1.5);
            // Strong iridescent mirror effect
            float fresnel = pow(1.0 - NdotV, 2.0);  // Broader fresnel for more color spread
            // Replace F entirely with strong iridescent reflection
            F = iridescence * (0.6 + fresnel * 0.4);
        }

        // Specular contribution
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        // Energy conservation: diffuse and specular must not exceed 1.0
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        // Metals have no diffuse reflection
        kD *= 1.0 - metallicMap;

        // Lambertian diffuse
        float NdotL = max(dot(N, L), 0.0);

        // Shadow calculation for directional lights
        float shadow = 1.0;
        if (lights[i].type == 0) {  // LIGHT_DIRECTIONAL = 0
            int shadowSlot = getShadowSlot(i);
            if (shadowSlot >= 0) {
                shadow = calculateShadow(shadowSlot, WorldPos, NdotL);
            }
        }

        // Add this light's contribution with shadow
        Lo += (kD * albedoMap / PI + specular) * radiance * NdotL * shadow;

        // Add subsurface scattering contribution
        if (subsurfaceTexExists > 0 && sssThickness < 0.99) {
            Lo += subsurfaceScattering(N, L, V, albedoMap, sssThickness, lights[i].color * lights[i].intensity * attenuation);
        }
    }

    // Ambient lighting with IBL
    vec3 ambient;
    if (iblEnabled > 0) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = fresnelSchlickRoughness(NdotV, F0, roughnessMap);

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallicMap;

        // Diffuse IBL: sample irradiance map with surface normal
        vec3 irradiance = texture(irradianceMap, N).rgb;
        vec3 diffuse = irradiance * albedoMap;

        // Specular IBL: sample prefiltered env map with reflection vector
        vec3 R = reflect(-V, N);
        vec3 prefilteredColor = textureLod(prefilteredMap, R, roughnessMap * maxReflectionLOD).rgb;
        vec2 brdf = texture(brdfLUT, vec2(NdotV, roughnessMap)).rg;
        vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);

        ambient = (kD * diffuse + specular) * aoMap * iblIntensity;
    } else {
        // Fallback to simple ambient when IBL is disabled
        ambient = vec3(0.03) * albedoMap * aoMap;
    }

    // Final color
    vec3 color = ambient + Lo + emissiveMap;

    // HDR tonemapping (Reinhard)
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = linearToSRGB(color);

    // For translucent materials, apply Fresnel-based alpha
    // Edges become more reflective (less transparent) at glancing angles
    float finalOpacity = opacity;
    if (opacity < 1.0) {
        float NdotV = max(dot(N, V), 0.0);
        float fresnelOpacity = iorF0 + (1.0 - iorF0) * pow(1.0 - NdotV, 5.0);
        // Blend between base opacity and full opacity based on Fresnel
        finalOpacity = mix(opacity, 1.0, fresnelOpacity);
    }

    FragColor = vec4(color, finalOpacity);
}
