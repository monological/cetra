#version 330 core
in vec3 Normal;
in vec3 FragPos;
in vec2 TexCoords;

out vec4 FragColor;

struct Light {
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
    int type; // LightType: 0 - Directional, 1 - Point, 2 - Spot, 3 - Unknown
};

uniform Light light;
uniform vec3 albedo;
uniform float metallic;
uniform float roughness;
uniform float ao;
uniform vec3 camPos;
uniform sampler2D albedoTex;
uniform sampler2D normalTex;
uniform sampler2D roughnessTex;
uniform sampler2D metalnessTex;
uniform sampler2D aoTex;
uniform sampler2D emissiveTex;
uniform sampler2D heightTex;

uniform int albedoTexExists;
uniform int normalTexExists;
uniform int roughnessTexExists;
uniform int metalnessTexExists;
uniform int aoTexExists;
uniform int emissiveTexExists;
uniform int heightTexExists;

const float PI = 3.14159265359;


float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2  = geometrySchlickGGX(NdotV, roughness);
    float ggx1  = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}


vec3 calculateLightContribution(vec3 N, vec3 V, vec3 L, float NdotL) {
    vec3 H = normalize(V + L);
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);

    vec3 nominator = NdotL * F * D * G;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    vec3 specular = nominator / max(denominator, 0.01);

    return specular;
}

vec3 calculatePointLight(vec3 N, vec3 V, Light light, vec3 fragPos) {
    vec3 L = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL);
    return lightContribution * light.color * light.intensity * attenuation;
}

vec3 calculateDirectionalLight(vec3 N, vec3 V, Light light) {
    vec3 L = normalize(-light.direction);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL);
    return lightContribution * light.color * light.intensity;
}

vec3 calculateSpotLight(vec3 N, vec3 V, Light light, vec3 fragPos) {
    vec3 L = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float theta = dot(L, normalize(-light.direction));
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL);
    return lightContribution * light.color * light.intensity * attenuation * intensity;
}

float textureExists(int texExists) {
    return float(texExists);
}

void main() {
    vec3 albedoMap = albedo;
    if (albedoTexExists > 0) {
        albedoMap = texture(albedoTex, TexCoords).rgb;
    }

    vec3 normalMap = normalize(Normal);
    if (normalTexExists > 0) {
        normalMap = normalize(texture(normalTex, TexCoords).rgb * 2.0 - 1.0);
    }

    float roughnessMap = roughness;
    if (roughnessTexExists > 0) {
        roughnessMap = texture(roughnessTex, TexCoords).r;
    }

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
        emissiveMap = texture(emissiveTex, TexCoords).rgb;
    }

    // Use declared variables for final values
    vec3 finalAlbedo = albedoMap;
    float finalMetallic = metallicMap;
    float finalRoughness = roughnessMap;
    float finalAO = aoMap;

    // Calculate PBR Lighting
    vec3 N = normalize(normalMap);
    vec3 V = normalize(camPos - FragPos);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, finalAlbedo, finalMetallic);

    vec3 Lo = vec3(0.0);
    // Light calculations (unchanged)

    vec3 ambient = finalAlbedo * finalAO;
    vec3 color = ambient + Lo + emissiveMap;

    // Tone Mapping
    color = color / (color + vec3(1.0)); // Reinhard tone mapping
    color = pow(color, vec3(1.0/2.2)); // Gamma Correction

    // Tone mapping and gamma correction (unchanged)
    FragColor = vec4(color, 1.0);
}


