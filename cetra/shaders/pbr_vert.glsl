#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec3 aBitangent;
layout(location = 5) in vec4 aColor;
layout(location = 8) in vec2 aTexCoords2;

out vec3 Normal;
out vec3 WorldPos;     // World position
out vec3 ViewPos;      // View position
out vec3 FragPos;      // Fragment position in clip space
out float ClipDepth;   // Depth in clip space
out float FragDepth;
out vec2 TexCoords;
out vec2 TexCoords2;   // UV1 for lightmaps/AO
out vec4 VertexColor;  // Vertex color (RGBA)
out mat3 TBN;
out vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
out vec4 PrevClip;     // Previous-frame clip position

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
uniform mat4 projection; // Jittered when TAA is on (rasterization only)

// Motion-vector inputs (un-jittered). Default to zero when unset, yielding zero
// velocity, which the TAA resolve treats as static.
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;

uniform vec3 camPos;
uniform float time;

void main() {

    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos = worldPos.xyz;

    // Motion vectors: current vs previous clip position, both un-jittered.
    CurrClip = uCurrViewProjNoJitter * worldPos;
    PrevClip = uPrevViewProj * uPrevModel * vec4(aPos, 1.0);

    vec4 viewPos = view * worldPos;
    ViewPos = viewPos.xyz;

    vec4 clipPos = projection * viewPos;
    FragPos = clipPos.xyz;
    ClipDepth = clipPos.z; // Depth in clip space

    FragDepth = gl_Position.z / gl_Position.w; // Perspective divide to get normalized device coordinates

    Normal = normalize(mat3(transpose(inverse(model))) * aNormal);
    TexCoords = aTexCoords;
    TexCoords2 = aTexCoords2;
    VertexColor = aColor;

    // Calculate the TBN matrix
    vec3 T = normalize(mat3(model) * aTangent);
    vec3 B = normalize(mat3(model) * aBitangent);
    vec3 N = normalize(mat3(model) * aNormal);
    TBN = mat3(T, B, N);


    


    gl_Position = clipPos;
}


