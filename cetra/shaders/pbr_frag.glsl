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

const float defaultEfficacy = 80.0; // 80 lumens per watt for LEDs


vec3 sRGBToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 applyGammaCorrection(vec3 color, float gamma) {
    return pow(color, vec3(1.0 / gamma));
}

float calculateAttenuation(float distance, float constant, float linear, float quadratic) {
    return 1.0 / (constant + linear * distance + quadratic * (distance * distance));
}

vec3 calculateDiffuse(vec3 N, vec3 L, vec3 lightColor, float distance, float constant, float linear, float quadratic) {
    float diff = max(dot(N, L), 0.0);
    float attenuation = calculateAttenuation(distance, constant, linear, quadratic);
    return diff * lightColor * attenuation;
}

void main() {
    vec3 albedoMap = vec3(1.0);  // Default white color
    if (albedoTexExists > 0) {
        albedoMap = texture(albedoTex, TexCoords).rgb;
        albedoMap = sRGBToLinear(albedoMap);  // Convert from sRGB to Linear RGB
    }

	vec3 normalMap;
	if (normalTexExists > 0 && false) {
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

    // Normalize the normal vectors
    vec3 N = normalize(normalMap);
    vec3 V = normalize(camPos - FragPos);
    vec3 L = normalize(lights[0].position - FragPos);
    float distance = length(lights[0].position - FragPos);

    // Light parameters
    float constant = lights[0].constant;
    float linear = lights[0].linear;
    float quadratic = lights[0].quadratic;
    vec3 lightColor = lights[0].color * lights[0].intensity;

    // Adjust ambient light intensity
    vec3 ambient = finalAlbedo * 0.09;
										   
	// Apply albedo to diffuse
    vec3 diffuse = calculateDiffuse(N, L, lightColor, distance, constant, linear, quadratic) * finalAlbedo;

    vec3 color = ambient + diffuse;
    color = clamp(color, 0.0, 1.0);

    color = applyGammaCorrection(color, 2.2);  // Apply gamma correction

    FragColor = vec4(color, 1.0);
}

