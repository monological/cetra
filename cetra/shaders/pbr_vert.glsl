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

// Directional wind (wind.h) -- global scene field + per-material response.
uniform float uDeltaTime;      // for the previous-frame position (motion vectors)
uniform vec3 uWindDir;         // world-space blow direction
uniform float uWindStrength;   // 0 = no wind on this scene
uniform float uWindSpeed;
uniform float uWindGustFreq;
uniform float uWindGustAmount;
uniform float uWindTurbulence;
uniform float uWindResponse;   // 0 = this material is rigid
uniform float uWindMaskMinY;   // local-space AABB Y bounds of the cloth mesh
uniform float uWindMaskMaxY;

// World-Position Offset wind for cloth (curtains). Zero unless the scene has
// wind (uWindStrength>0) and this material opted in (uWindResponse>0). The mask
// pins the top (maxY) and lets the hem (minY) swing; the gust envelope keeps it
// mostly calm with occasional swells. `p` is the object-space vertex position.
vec3 windOffset(vec3 p, float t) {
    if (uWindStrength <= 0.0 || uWindResponse <= 0.0)
        return vec3(0.0);
    float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
    float h = clamp((uWindMaskMaxY - p.y) / denom, 0.0, 1.0);
    float mask = h * h; // more sway toward the hem
    float gust = mix(1.0 - uWindGustAmount, 1.0, pow(0.5 + 0.5 * sin(t * uWindGustFreq), 3.0));
    float ph = t * uWindSpeed + p.y * 2.0 + p.x * 1.3;
    float sway = 0.5 + 0.5 * sin(ph); // 0..1 forward billow, never past rest
    float amp = uWindStrength * uWindResponse * mask * gust;
    vec3 flutter = vec3(sin(ph * 3.1), 0.0, cos(ph * 2.7)) * (uWindTurbulence * amp * 0.3);
    return normalize(uWindDir) * (sway * amp) + flutter;
}

void main() {

    // Wind displaces the object-space position; the previous-frame position uses
    // t - dt so the motion vector stays honest (no TAA/motion-blur smear).
    vec3 posCurr = aPos + windOffset(aPos, time);
    vec3 posPrev = aPos + windOffset(aPos, time - uDeltaTime);

    vec4 worldPos = model * vec4(posCurr, 1.0);
    WorldPos = worldPos.xyz;

    // Motion vectors: current vs previous clip position, both un-jittered.
    CurrClip = uCurrViewProjNoJitter * worldPos;
    PrevClip = uPrevViewProj * uPrevModel * vec4(posPrev, 1.0);

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


