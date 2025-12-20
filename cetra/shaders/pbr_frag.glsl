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

    // Calculate view direction
    vec3 V = normalize(camPos - FragPos);

    // Calculate F0 (surface reflection at zero incidence)
    // Dielectrics use 0.04, metals use albedo color
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedoMap, metallicMap);

    // Accumulate lighting from all lights
    vec3 Lo = vec3(0.0);

    for (int i = 0; i < numLights; i++) {
        // Calculate per-light radiance
        vec3 L = normalize(lights[i].position - FragPos);
        vec3 H = normalize(V + L);
        float distance = length(lights[i].position - FragPos);

        float attenuation = calculateAttenuation(distance, lights[i].constant,
                                                  lights[i].linear, lights[i].quadratic);
        vec3 radiance = lights[i].color * lights[i].intensity * attenuation;

        // Cook-Torrance BRDF
        float NDF = distributionGGX(N, H, roughnessMap);
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

        // Add this light's contribution
        Lo += (kD * albedoMap / PI + specular) * radiance * NdotL;
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
