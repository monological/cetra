#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;

out vec3 Normal;
out vec3 WorldPos;     // World position
out vec3 ViewPos;      // View position
out vec3 FragPos;      // Fragment position in clip space
out float ClipDepth;   // Depth in clip space
out float FragDepth;
out vec2 TexCoords;
out mat3 TBN;

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

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform vec3 camPos;
uniform float time;

void main() {

    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos = worldPos.xyz;
    
    vec4 viewPos = view * worldPos;
    ViewPos = viewPos.xyz;

    vec4 clipPos = projection * viewPos;
    FragPos = clipPos.xyz;
    ClipDepth = clipPos.z; // Depth in clip space

    FragDepth = gl_Position.z / gl_Position.w; // Perspective divide to get normalized device coordinates

    Normal = normalize(mat3(transpose(inverse(model))) * aNormal);
    TexCoords = aTexCoords;

    // Calculate the TBN matrix
    vec3 T = normalize(mat3(model) * aTangent);
    vec3 B = normalize(mat3(model) * aBitangent);
    vec3 N = normalize(mat3(model) * aNormal);
    TBN = mat3(T, B, N);


    


    gl_Position = clipPos;
}


