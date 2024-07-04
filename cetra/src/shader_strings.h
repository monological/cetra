#ifndef SHADER_STRINGS_H
#define SHADER_STRINGS_H

static const char* shape_geo_shader_str = 
    "#version 400 core\n"
    "layout(lines) in;\n"
    "layout(triangle_strip, max_vertices = 6) out;\n"
    ""
    "in vec3 WorldPos_vs[2]; // World position from vertex shader\n"
    ""
    "uniform mat4 projection;\n"
    "uniform mat4 view;\n"
    "uniform float lineWidth;\n"
    "uniform float time;\n"
    ""
    "void main() {\n"
    "    vec3 startPosition = WorldPos_vs[0];\n"
    "    vec3 endPosition = WorldPos_vs[1];\n"
    ""
    "    // Direction and perpendicular vector\n"
    "    vec3 lineDir = normalize(endPosition - startPosition);\n"
    "    vec3 perpVec = normalize(vec3(-lineDir.y, lineDir.x, 0.0)) * lineWidth * 0.5;\n"
    ""
    "    // Offset vector: a small fraction of the line direction\n"
    "    vec3 offsetVec = lineDir * 0.12 * lineWidth; // Adjust this value as needed\n"
    ""
    "    gl_Position = projection * view * vec4(startPosition + perpVec - offsetVec, 1.0);\n"
    "    EmitVertex();\n"
    "    gl_Position = projection * view * vec4(startPosition - perpVec - offsetVec, 1.0);\n"
    "    EmitVertex();\n"
    "    gl_Position = projection * view * vec4(endPosition + perpVec + offsetVec, 1.0);\n"
    "    EmitVertex();\n"
    "    gl_Position = projection * view * vec4(endPosition - perpVec + offsetVec, 1.0);\n"
    "    EmitVertex();\n"
    ""
    "    EndPrimitive();\n"
    "}\n"
    ""
    "";

static const char* pbr_vert_shader_str = 
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aTexCoords;\n"
    "layout(location = 3) in vec3 aTangent;\n"
    "layout(location = 4) in vec3 aBitangent;\n"
    ""
    "out vec3 Normal;\n"
    "out vec3 WorldPos;     // World position\n"
    "out vec3 ViewPos;      // View position\n"
    "out vec3 FragPos;      // Fragment position in clip space\n"
    "out float ClipDepth;   // Depth in clip space\n"
    "out float FragDepth;\n"
    "out vec2 TexCoords;\n"
    "out mat3 TBN;\n"
    ""
    "#define MAX_LIGHTS 75\n"
    ""
    "struct Light {\n"
    "    int type;\n"
    "    vec3 position;\n"
    "    vec3 direction;\n"
    "    vec3 color;\n"
    "    vec3 specular;\n"
    "    vec3 ambient;\n"
    "    float intensity;\n"
    "    float constant;\n"
    "    float linear;\n"
    "    float quadratic;\n"
    "    float cutOff;\n"
    "    float outerCutOff;\n"
    "    vec2 size;\n"
    "};\n"
    ""
    "uniform Light lights[MAX_LIGHTS];\n"
    "uniform int numLights;\n"
    ""
    "uniform mat4 model;\n"
    "uniform mat4 view;\n"
    "uniform mat4 projection;\n"
    ""
    "uniform vec3 camPos;\n"
    "uniform float time;\n"
    ""
    "void main() {\n"
    ""
    "    vec4 worldPos = model * vec4(aPos, 1.0);\n"
    "    WorldPos = worldPos.xyz;\n"
    ""
    "    vec4 viewPos = view * worldPos;\n"
    "    ViewPos = viewPos.xyz;\n"
    ""
    "    vec4 clipPos = projection * viewPos;\n"
    "    FragPos = clipPos.xyz;\n"
    "    ClipDepth = clipPos.z; // Depth in clip space\n"
    ""
    "    FragDepth = gl_Position.z / gl_Position.w; // Perspective divide to get normalized device coordinates\n"
    ""
    "    Normal = normalize(mat3(transpose(inverse(model))) * aNormal);\n"
    "    TexCoords = aTexCoords;\n"
    ""
    "    // Calculate the TBN matrix\n"
    "    vec3 T = normalize(mat3(model) * aTangent);\n"
    "    vec3 B = normalize(mat3(model) * aBitangent);\n"
    "    vec3 N = normalize(mat3(model) * aNormal);\n"
    "    TBN = mat3(T, B, N);\n"
    ""
    ""
    ""
    ""
    ""
    "    gl_Position = clipPos;\n"
    "}\n"
    ""
    "";

static const char* shape_frag_shader_str = 
    "#version 410 core\n"
    ""
    "out vec4 FragColor;\n"
    ""
    "uniform vec3 albedo;\n"
    ""
    "void main()\n"
    "{\n"
    "    FragColor = vec4(albedo, 1.0);\n"
    "}\n"
    "";

static const char* pbr_frag_shader_str = 
    "#version 330 core\n"
    "in vec3 Normal;\n"
    "in vec3 WorldPos;\n"
    "in vec3 ViewPos;\n"
    "in vec3 FragPos;\n"
    "in float ClipDepth;\n"
    "in float FragDepth;\n"
    "in vec2 TexCoords;\n"
    "in mat3 TBN;\n"
    "out vec4 FragColor;\n"
    ""
    "#define MAX_LIGHTS 75\n"
    ""
    "struct Light {\n"
    "    int type;\n"
    "    vec3 position;\n"
    "    vec3 direction;\n"
    "    vec3 color;\n"
    "    vec3 specular;\n"
    "    vec3 ambient;\n"
    "    float intensity;\n"
    "    float constant;\n"
    "    float linear;\n"
    "    float quadratic;\n"
    "    float cutOff;\n"
    "    float outerCutOff;\n"
    "    vec2 size;\n"
    "};\n"
    ""
    "uniform Light lights[MAX_LIGHTS];\n"
    "uniform int numLights;\n"
    ""
    "uniform mat4 view;\n"
    "uniform mat4 model;\n"
    "uniform mat4 projection;\n"
    ""
    "uniform int renderMode;\n"
    "uniform float nearClip;\n"
    "uniform float farClip;\n"
    ""
    "uniform vec3 albedo;\n"
    "uniform float metallic;\n"
    "uniform float roughness;\n"
    "uniform float ao;\n"
    "uniform vec3 camPos;\n"
    "uniform float time;\n"
    ""
    "uniform sampler2D albedoTex;\n"
    "uniform sampler2D normalTex;\n"
    "uniform sampler2D roughnessTex;\n"
    "uniform sampler2D metalnessTex;\n"
    "uniform sampler2D aoTex;\n"
    "uniform sampler2D emissiveTex;\n"
    "uniform sampler2D heightTex;\n"
    "uniform sampler2D opacityTex;\n"
    "uniform sampler2D sheenTex;\n"
    "uniform sampler2D reflectanceTex;\n"
    ""
    "uniform int albedoTexExists;\n"
    "uniform int normalTexExists;\n"
    "uniform int roughnessTexExists;\n"
    "uniform int metalnessTexExists;\n"
    "uniform int aoTexExists;\n"
    "uniform int emissiveTexExists;\n"
    "uniform int heightTexExists;\n"
    "uniform int opacityTexExists;\n"
    "uniform int sheenTexExists;\n"
    "uniform int reflectanceTexExists;\n"
    ""
    "const float PI = 3.14159265359;\n"
    ""
    "const float defaultEfficacy = 80.0; // 80 lumens per watt for LEDs\n"
    ""
    "float distributionGGX(vec3 N, vec3 H, float roughness, float gamma) {\n"
    "    float a = pow(roughness, gamma); // Non-linear mapping of roughness\n"
    "    float a2 = a * a;\n"
    "    float NdotH = max(dot(N, H), 0.0);\n"
    "    float NdotH2 = NdotH * NdotH;\n"
    ""
    "    float nom = a2;\n"
    "    float denom = (NdotH2 * (a2 - 1.0) + 1.0);\n"
    "    denom = PI * denom * denom;\n"
    ""
    "    return nom / denom;\n"
    "}\n"
    ""
    ""
    "float geometrySchlickGGX(float NdotV, float roughness) {\n"
    "    float r = (roughness + 1.0);\n"
    "    float k = (r*r) / 8.0;\n"
    ""
    "    float nom   = NdotV;\n"
    "    float denom = NdotV * (1.0 - k) + k;\n"
    ""
    "    return nom / denom;\n"
    "}\n"
    ""
    "float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {\n"
    "    float NdotV = max(dot(N, V), 0.0);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    float ggx2  = geometrySchlickGGX(NdotV, roughness);\n"
    "    float ggx1  = geometrySchlickGGX(NdotL, roughness);\n"
    ""
    "    return ggx1 * ggx2;\n"
    "}\n"
    ""
    "vec3 fresnelSchlick(float cosTheta, vec3 F0, float metallic) {\n"
    "    vec3 F0_non_metal = vec3(0.04); // Base reflectivity for non-metals\n"
    "    F0 = mix(F0_non_metal, F0, metallic); // Interpolate based on metallic value\n"
    "    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);\n"
    "}\n"
    ""
    "vec3 convertWattsToRadiance(vec3 power, float efficacy) {\n"
    "    float lumensPerWatt = efficacy;\n"
    "    vec3 lumens = power * lumensPerWatt;\n"
    "    // Assuming a standard area and solid angle, or adjust as needed\n"
    "    float area = 100.0; // in square meters\n"
    "    vec3 radiance = lumens / area;\n"
    "    return radiance;\n"
    "}\n"
    ""
    ""
    "vec3 calculateLightContribution(vec3 N, vec3 V, vec3 L, float NdotL, float metallic, vec3 albedo) {\n"
    "    vec3 H = normalize(V + L);\n"
    "    vec3 F0 = albedo; // Reflectivity at normal incidence for metals\n"
    ""
    "    // Calculate Fresnel term using updated function\n"
    "    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0, metallic);\n"
    ""
    "    float gamma = 2.0;\n"
    "    float D = distributionGGX(N, H, roughness, gamma);\n"
    "    float G = geometrySmith(N, V, L, roughness);\n"
    ""
    "    vec3 nominator = NdotL * F * D * G;\n"
    "    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);\n"
    "    vec3 specular = nominator / max(denominator, 0.01);\n"
    ""
    "    return specular;\n"
    "}\n"
    ""
    "vec3 calculatePointLight(vec3 N, vec3 V, Light light, vec3 fragPos, float metallic, vec3 albedo, vec3 radiance) {\n"
    "    vec3 L = normalize(light.position - fragPos);\n"
    "    float distance = length(light.position - fragPos);\n"
    "    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);\n"
    "    return lightContribution * radiance * light.intensity * attenuation;\n"
    "}\n"
    ""
    "vec3 calculateDirectionalLight(vec3 N, vec3 V, Light light, float metallic, vec3 albedo, vec3 radiance) {\n"
    "    vec3 L = normalize(-light.direction);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);\n"
    "    return lightContribution * radiance * light.intensity;\n"
    "}\n"
    ""
    "vec3 calculateSpotLight(vec3 N, vec3 V, Light light, vec3 fragPos, float metallic, vec3 albedo, vec3 radiance) {\n"
    "    vec3 L = normalize(light.position - fragPos);\n"
    "    float distance = length(light.position - fragPos);\n"
    "    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));\n"
    "    float theta = dot(L, normalize(-light.direction));\n"
    "    float epsilon = light.cutOff - light.outerCutOff;\n"
    "    float intensity = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    "    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);\n"
    "    return lightContribution * radiance * light.intensity * attenuation * intensity;\n"
    "}\n"
    ""
    "// GLSL\n"
    ""
    "vec3 calculateAreaLight(vec3 N, vec3 V, Light light, vec3 fragPos, float metallic, vec3 albedo, vec3 radiance) {\n"
    "    // Basic properties\n"
    "    vec3 L = normalize(light.position - fragPos); // Direction from fragment to light\n"
    "    float distance = length(light.position - fragPos);\n"
    "    float NdotL = max(dot(N, L), 0.0);\n"
    ""
    "    // Area light specific calculations\n"
    "    // Assuming 'size' is a vec2 representing the width and height of the light\n"
    "    vec2 size = light.size;\n"
    "    vec3 lightRight = normalize(cross(light.direction, vec3(0.0, 1.0, 0.0))); // Adjust as needed\n"
    "    vec3 lightUp = cross(light.direction, lightRight);\n"
    ""
    "    // Simple form factor calculation for rectangular area light\n"
    "    float formFactor = 0.0;\n"
    "    for (int i = -1; i <= 1; i += 2) {\n"
    "        for (int j = -1; j <= 1; j += 2) {\n"
    "            vec3 cornerDir = normalize(light.position + (lightRight * size.x * i + lightUp * size.y * j) - fragPos);\n"
    "            formFactor += max(dot(N, cornerDir), 0.0);\n"
    "        }\n"
    "    }\n"
    "    formFactor *= 0.25;\n"
    ""
    "    // Light attenuation (modify as per your attenuation model)\n"
    "    float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));\n"
    ""
    "    // Calculate light contribution using the PBR model\n"
    "    vec3 lightContribution = calculateLightContribution(N, V, L, NdotL, metallic, albedo);\n"
    ""
    "    return lightContribution * radiance * light.intensity * attenuation * formFactor;\n"
    "}\n"
    ""
    ""
    "float textureExists(int texExists) {\n"
    "    return float(texExists);\n"
    "}\n"
    ""
    ""
    "float linearizeDepth(float depth) {\n"
    "    float z = depth * 2.0 - 1.0; // Back to NDC\n"
    "    return (2.0 * nearClip * farClip) / (farClip + nearClip - z * (farClip - nearClip));\n"
    "}\n"
    ""
    ""
    "void main() {\n"
    ""
    "    if(renderMode == 0){ // PBR\n"
    ""
    "        vec3 albedoMap = albedo;\n"
    "        if (albedoTexExists > 0) {\n"
    "            albedoMap = texture(albedoTex, TexCoords).rgb;\n"
    "        }\n"
    ""
    "        // Sample and transform the normal using the TBN matrix\n"
    "        vec3 normalMap;\n"
    "        if (normalTexExists > 0) {\n"
    "            normalMap = texture(normalTex, TexCoords).rgb;\n"
    "            // Transform normal from [0,1] range to [-1,1] range and apply TBN matrix\n"
    "            normalMap = normalMap * 2.0 - 1.0;\n"
    "            normalMap = normalize(TBN * normalMap);\n"
    "        } else {\n"
    "            normalMap = normalize(Normal); // Use vertex normal if no normal map\n"
    "        }\n"
    ""
    "        float roughnessMap = roughness;\n"
    "        if (roughnessTexExists > 0) {\n"
    "            roughnessMap = texture(roughnessTex, TexCoords).r;\n"
    "        }\n"
    ""
    "        float metallicMap = metallic;\n"
    "        if (metalnessTexExists > 0) {\n"
    "            metallicMap = texture(metalnessTex, TexCoords).r;\n"
    "        }\n"
    ""
    "        float aoMap = ao;\n"
    "        if (aoTexExists > 0) {\n"
    "            aoMap = texture(aoTex, TexCoords).r;\n"
    "        }\n"
    ""
    "        vec3 emissiveMap = vec3(0.0);\n"
    "        if (emissiveTexExists > 0) {\n"
    "            emissiveMap = texture(emissiveTex, TexCoords).rgb;\n"
    "        }\n"
    ""
    "        float opacity = 1.0;\n"
    "        if (opacityTexExists > 0) {\n"
    "            opacity = texture(opacityTex, TexCoords).r;\n"
    "        }\n"
    ""
    "        vec3 sheenColor = vec3(0.0);\n"
    "        if (sheenTexExists > 0) {\n"
    "            sheenColor = texture(sheenTex, TexCoords).rgb;\n"
    "        }\n"
    ""
    "        vec3 reflectance = vec3(0.04); // Default reflectance value\n"
    "        if (reflectanceTexExists > 0) {\n"
    "            reflectance = texture(reflectanceTex, TexCoords).rgb;\n"
    "        }\n"
    ""
    "        // Use declared variables for final values\n"
    "        vec3 finalAlbedo = albedoMap;\n"
    "        float finalMetallic = metallicMap;\n"
    "        float finalRoughness = roughnessMap;\n"
    "        float finalAO = aoMap;\n"
    ""
    "        // Calculate PBR Lighting\n"
    "        vec3 N = normalize(normalMap);\n"
    "        vec3 V = normalize(camPos - FragPos);\n"
    "        vec3 F0 = mix(vec3(0.04), albedoMap, metallicMap);\n"
    ""
    "        F0 = mix(F0, finalAlbedo, finalMetallic);\n"
    ""
    "        vec3 Lo = vec3(0.0); // Accumulate specular component\n"
    ""
    "        for (int i = 0; i < numLights; i++) {\n"
    "            Light light = lights[i];\n"
    ""
    "            vec3 radiance = convertWattsToRadiance(light.color, defaultEfficacy);\n"
    ""
    "            if (light.type == 0) { // Directional light\n"
    "                Lo += calculateDirectionalLight(N, V, light, finalMetallic, finalAlbedo, radiance);\n"
    "            } else if (light.type == 1) { // Point light\n"
    "                Lo += calculatePointLight(N, V, light, FragPos, finalMetallic, finalAlbedo, radiance);\n"
    "            } else if (light.type == 2) { // Spot light\n"
    "                Lo += calculateSpotLight(N, V, light, FragPos, finalMetallic, finalAlbedo, radiance);\n"
    "            }else if (light.type == 3) { // Area light\n"
    "                Lo += calculateAreaLight(N, V, light, FragPos, finalMetallic, finalAlbedo, radiance);\n"
    "            }\n"
    "        }\n"
    ""
    "        // Ambient and diffuse calculation\n"
    "        vec3 ambient = finalAlbedo * finalAO;\n"
    "        vec3 diffuse = (1.0 - finalMetallic) * finalAlbedo / PI;\n"
    ""
    "        // Combine lighting components\n"
    "        vec3 color = ambient + Lo + diffuse + emissiveMap + sheenColor;\n"
    ""
    "        // Apply reflectance\n"
    "        color = mix(color, vec3(1.0), reflectance);\n"
    ""
    "        // Apply opacity to the final color\n"
    "        color = mix(vec3(0.0), color, opacity);\n"
    ""
    "        // Tone Mapping\n"
    "        color = color / (color + vec3(1.0)); // Reinhard tone mapping\n"
    "        color = pow(color, vec3(1.0/1.5)); // Gamma Correction\n"
    ""
    "        FragColor = vec4(color, 1.0);\n"
    "    }else if (renderMode == 1) { // Normals Visualization\n"
    "        vec3 color = normalize(Normal) * 0.5 + 0.5;\n"
    "        FragColor = vec4(color, 1.0);\n"
    "    }\n"
    "    else if (renderMode == 2) { // World Position Visualization\n"
    "        vec3 color = 0.5 * WorldPos + 0.5;\n"
    "        FragColor = vec4(color, 1.0);\n"
    "    }\n"
    "    else if (renderMode == 3) { // Texture Coordinates Visualization\n"
    "        FragColor = vec4(TexCoords, 0.0, 1.0);\n"
    "    }\n"
    "    else if (renderMode == 4) { // Tangent Space Visualization\n"
    "        vec3 tangent = normalize(TBN[0]);\n"
    "        vec3 bitangent = normalize(TBN[1]);\n"
    "        vec3 normal = normalize(TBN[2]);\n"
    "        FragColor = vec4(tangent * 0.5 + 0.5, 1.0);\n"
    "    } else if (renderMode == 5) { // Flat color\n"
    "        FragColor = vec4(1.0f, 0.0f, 0.0f, 1.0);\n"
    "    }\n"
    ""
    ""
    ""
    "}\n"
    ""
    "";

static const char* shape_vert_shader_str = 
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aTexCoords;\n"
    "layout(location = 3) in vec3 aTangent;\n"
    "layout(location = 4) in vec3 aBitangent;\n"
    ""
    "out vec3 Normal_vs;\n"
    "out vec3 WorldPos_vs;     // World position\n"
    "out vec3 ViewPos_vs;      // View position\n"
    "out vec3 FragPos_vs;      // Fragment position in clip space\n"
    "out float ClipDepth_vs;   // Depth in clip space\n"
    "out float FragDepth_vs;\n"
    "out vec2 TexCoords_vs;\n"
    "out mat3 TBN_vs;\n"
    ""
    "#define MAX_LIGHTS 75\n"
    ""
    "struct Light {\n"
    "    int type;\n"
    "    vec3 position;\n"
    "    vec3 direction;\n"
    "    vec3 color;\n"
    "    vec3 specular;\n"
    "    vec3 ambient;\n"
    "    float intensity;\n"
    "    float constant;\n"
    "    float linear;\n"
    "    float quadratic;\n"
    "    float cutOff;\n"
    "    float outerCutOff;\n"
    "    vec2 size;\n"
    "};\n"
    ""
    "uniform Light lights[MAX_LIGHTS];\n"
    "uniform int numLights;\n"
    ""
    "uniform mat4 model;\n"
    "uniform mat4 view;\n"
    "uniform mat4 projection;\n"
    ""
    "uniform vec3 camPos;\n"
    "uniform float time;\n"
    ""
    "void main() {\n"
    ""
    "    vec4 worldPos = model * vec4(aPos, 1.0);\n"
    "    WorldPos_vs = worldPos.xyz;\n"
    ""
    "    vec4 viewPos = view * worldPos;\n"
    "    ViewPos_vs = viewPos.xyz;\n"
    ""
    "    vec4 clipPos = projection * viewPos;\n"
    "    FragPos_vs = clipPos.xyz;\n"
    "    ClipDepth_vs = clipPos.z; // Depth in clip space\n"
    ""
    "    // Perspective divide to get normalized device coordinates\n"
    "    FragDepth_vs = gl_Position.z / gl_Position.w;\n"
    ""
    "    Normal_vs = normalize(mat3(transpose(inverse(model))) * aNormal);\n"
    "    TexCoords_vs = aTexCoords;\n"
    ""
    "    // Calculate the TBN matrix\n"
    "    vec3 T = normalize(mat3(model) * aTangent);\n"
    "    vec3 B = normalize(mat3(model) * aBitangent);\n"
    "    vec3 N = normalize(mat3(model) * aNormal);\n"
    "    TBN_vs = mat3(T, B, N);\n"
    ""
    "    gl_Position = clipPos;\n"
    "}\n"
    ""
    "";

#endif // SHADER_STRINGS_H