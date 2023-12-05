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
};

uniform Light light;

uniform vec3 view;
uniform vec3 model;
uniform vec3 projection;

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

float distributionGGX(vec3 N, vec3 H, float roughness, float gamma) {
    float a = pow(roughness, gamma); // Non-linear mapping of roughness
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
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

vec3 fresnelSchlick(float cosTheta, vec3 F0, float metallic) {
    vec3 F0_non_metal = vec3(0.04); // Base reflectivity for non-metals
    F0 = mix(F0_non_metal, F0, metallic); // Interpolate based on metallic value
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}


vec3 calculateLightContribution(vec3 N, vec3 V, vec3 L, float NdotL, float metallic, vec3 albedo) {
    vec3 H = normalize(V + L);
    vec3 F0 = albedo; // Reflectivity at normal incidence for metals

    // Calculate Fresnel term using updated function
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0, metallic);

    float gamma = 2.0;
    float D = distributionGGX(N, H, roughness, gamma);
    float G = geometrySmith(N, V, L, roughness);

    vec3 nominator = NdotL * F * D * G;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    vec3 specular = nominator / max(denominator, 0.01);

    return specular;
}

vec3 calculatePointLight(vec3 N, vec3 V, Light light, vec3 fragPos, float metallic, vec3 albedo) {
    vec3 L = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);
    return lightContribution * light.color * light.intensity * attenuation;
}

vec3 calculateDirectionalLight(vec3 N, vec3 V, Light light, float metallic, vec3 albedo) {
    vec3 L = normalize(-light.direction);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);
    return lightContribution * light.color * light.intensity;
}

vec3 calculateSpotLight(vec3 N, vec3 V, Light light, vec3 fragPos, float metallic, vec3 albedo) {
    vec3 L = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
    float theta = dot(L, normalize(-light.direction));
    float epsilon = light.cutOff - light.outerCutOff;
    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
    float NdotL = max(dot(N, L), 0.0);
    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);
    return lightContribution * light.color * light.intensity * attenuation * intensity;
}

float textureExists(int texExists) {
    return float(texExists);
}


float linearizeDepth(float depth) {
    float z = depth * 2.0 - 1.0; // Back to NDC
    return (2.0 * nearClip * farClip) / (farClip + nearClip - z * (farClip - nearClip));
}


void main() {

    if(renderMode == 0){ // PBR

        vec3 albedoMap = albedo;
        if (albedoTexExists > 0) {
            albedoMap = texture(albedoTex, TexCoords).rgb;
        }

        // Sample and transform the normal using the TBN matrix
        vec3 normalMap;
        if (normalTexExists > 0) {
            normalMap = texture(normalTex, TexCoords).rgb;
            // Transform normal from [0,1] range to [-1,1] range and apply TBN matrix
            normalMap = normalMap * 2.0 - 1.0;
            normalMap = normalize(TBN * normalMap);
        } else {
            normalMap = normalize(Normal); // Use vertex normal if no normal map
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

        float opacity = 1.0;
        if (opacityTexExists > 0) {
            opacity = texture(opacityTex, TexCoords).r;
        }

        vec3 sheenColor = vec3(0.0);
        if (sheenTexExists > 0) {
            sheenColor = texture(sheenTex, TexCoords).rgb;
        }

        vec3 reflectance = vec3(0.04); // Default reflectance value
        if (reflectanceTexExists > 0) {
            reflectance = texture(reflectanceTex, TexCoords).rgb;
        }

        // Use declared variables for final values
        vec3 finalAlbedo = albedoMap;
        float finalMetallic = metallicMap;
        float finalRoughness = roughnessMap;
        float finalAO = aoMap;

        // Calculate PBR Lighting
        vec3 N = normalize(normalMap);
        vec3 V = normalize(camPos - FragPos);
        vec3 F0 = mix(vec3(0.04), albedoMap, metallicMap);

        F0 = mix(F0, finalAlbedo, finalMetallic);


        vec3 Lo = vec3(0.0); // Accumulate specular component
        if (light.type == 0) { // Directional light
            Lo += calculateDirectionalLight(N, V, light, finalMetallic, finalAlbedo);
        } else if (light.type == 1) { // Point light
            Lo += calculatePointLight(N, V, light, FragPos, finalMetallic, finalAlbedo);
        } else if (light.type == 2) { // Spot light
            Lo += calculateSpotLight(N, V, light, FragPos, finalMetallic, finalAlbedo);
        }

        // Ambient and diffuse calculation
        vec3 ambient = finalAlbedo * finalAO;
        vec3 diffuse = (1.0 - finalMetallic) * finalAlbedo / PI;

        // Combine lighting components
        vec3 color = ambient + Lo + diffuse + emissiveMap + sheenColor;

        // Apply reflectance
        color = mix(color, vec3(1.0), reflectance);

        // Apply opacity to the final color
        color = mix(vec3(0.0), color, opacity);

        // Tone Mapping
        color = color / (color + vec3(1.0)); // Reinhard tone mapping
        color = pow(color, vec3(1.0/2.2)); // Gamma Correction

        FragColor = vec4(color, 1.0);
    }else if (renderMode == 1) { // Normals Visualization
        vec3 color = normalize(Normal) * 0.5 + 0.5;
        FragColor = vec4(color, 1.0);
    }
    else if (renderMode == 2) { // World Position Visualization
        vec3 color = 0.5 * WorldPos + 0.5;
        FragColor = vec4(color, 1.0);
    }
    else if (renderMode == 3) { // Texture Coordinates Visualization
        FragColor = vec4(TexCoords, 0.0, 1.0);
    }
    else if (renderMode == 4) { // Tangent Space Visualization
        vec3 tangent = normalize(TBN[0]);
        vec3 bitangent = normalize(TBN[1]);
        vec3 normal = normalize(TBN[2]);
        FragColor = vec4(tangent * 0.5 + 0.5, 1.0);
    } else if (renderMode == 5) { // Flat color
        FragColor = vec4(1.0f, 0.0f, 0.0f, 1.0);
    }

    

}


