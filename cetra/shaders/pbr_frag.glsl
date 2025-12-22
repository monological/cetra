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

#define MAX_LIGHTS 75

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

    float opacity = 1.0;
    if (opacityTexExists > 0) {
        opacity = texture(opacityTex, TexCoords).r;
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

    // Calculate view direction
    vec3 V = normalize(camPos - FragPos);

    // Render mode handling
    if (renderMode == 1) {
        // Normals Visualization
        vec3 color = normalize(Normal) * 0.5 + 0.5;
        FragColor = vec4(color, 1.0);
        return;
    } else if (renderMode == 2) {
        // World Position Visualization
        vec3 color = fract(WorldPos * 0.01);
        FragColor = vec4(color, 1.0);
        return;
    } else if (renderMode == 3) {
        // Texture Coordinates Visualization
        FragColor = vec4(TexCoords, 0.0, 1.0);
        return;
    } else if (renderMode == 4) {
        // Tangent Space Visualization
        vec3 tangent = normalize(TBN[0]) * 0.5 + 0.5;
        FragColor = vec4(tangent, 1.0);
        return;
    } else if (renderMode == 5) {
        // Flat Color
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
        return;
    } else if (renderMode == 6) {
        // Albedo Only
        vec3 color = linearToSRGB(albedoMap);
        FragColor = vec4(color, opacity);
        return;
    } else if (renderMode == 7) {
        // Simple Diffuse Lighting
        vec3 Lo = vec3(0.0);
        for (int i = 0; i < numLights; i++) {
            vec3 L;
            float attenuation;
            if (lights[i].type == 0) {
                L = normalize(-lights[i].direction);
                attenuation = 1.0;
            } else {
                L = normalize(lights[i].position - FragPos);
                float distance = length(lights[i].position - FragPos);
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
    } else if (renderMode == 8) {
        // Metallic and Roughness Visualization
        FragColor = vec4(metallicMap, roughnessMap, 0.0, 1.0);
        return;
    }

    // renderMode == 0: Full PBR

    // Calculate F0 (surface reflection at zero incidence)
    // Dielectrics use 0.04, metals use albedo color
    vec3 F0 = vec3(0.04);
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
            L = normalize(lights[i].position - FragPos);
            float distance = length(lights[i].position - FragPos);
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

    // Ambient lighting (simplified IBL approximation)
    vec3 ambient = vec3(0.03) * albedoMap * aoMap;

    // Final color
    vec3 color = ambient + Lo + emissiveMap;

    // HDR tonemapping (Reinhard)
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = linearToSRGB(color);

    FragColor = vec4(color, opacity);
}
