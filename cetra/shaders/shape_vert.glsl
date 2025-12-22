#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;

out vec3 Normal_vs;
out vec3 WorldPos_vs;     // World position
out vec3 ViewPos_vs;      // View position
out vec3 FragPos_vs;      // Fragment position in clip space
out float ClipDepth_vs;   // Depth in clip space
out float FragDepth_vs;
out vec2 TexCoords_vs;
out mat3 TBN_vs;

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

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform vec3 camPos;
uniform float time;

void main() {

    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos_vs = worldPos.xyz;
    
    vec4 viewPos = view * worldPos;
    ViewPos_vs = viewPos.xyz;

    vec4 clipPos = projection * viewPos;
    FragPos_vs = clipPos.xyz;
    ClipDepth_vs = clipPos.z; // Depth in clip space

    // Perspective divide to get normalized device coordinates
    FragDepth_vs = gl_Position.z / gl_Position.w;

    Normal_vs = normalize(mat3(transpose(inverse(model))) * aNormal);
    TexCoords_vs = aTexCoords;

    // Calculate the TBN matrix
    vec3 T = normalize(mat3(model) * aTangent);
    vec3 B = normalize(mat3(model) * aBitangent);
    vec3 N = normalize(mat3(model) * aNormal);
    TBN_vs = mat3(T, B, N);

    gl_Position = clipPos;
}


