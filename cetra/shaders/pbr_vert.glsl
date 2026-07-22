#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // xyz tangent, w bitangent handedness
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
uniform int uWindMode;         // 0 = cloth, 1 = vegetation branch, 2 = vegetation leaf

// World-Position Offset wind. Zero unless the scene has wind (uWindStrength>0)
// and this material opted in (uWindResponse>0). `p` is the object-space vertex
// position, `uv0`/`uv1` its texture coordinates, `t` the time to evaluate at.
//
// Cloth (uWindMode 0, curtains): the mask pins the top (maxY) and lets the hem
// (minY) swing; the gust envelope keeps it mostly calm with occasional swells.
//
// Vegetation (uWindMode 1-2) reads UV1: .x is a per-branch phase in [0,1) that
// de-phases neighbouring branches, .y is a flex weight running 0 at the trunk
// base to 1 at the tips. Both are continuous across branch joints, so the
// displacement cannot tear the mesh where a child branch meets its parent.
vec3 windOffset(vec3 p, vec2 uv0, vec2 uv1, float t) {
    if (uWindStrength <= 0.0 || uWindResponse <= 0.0)
        return vec3(0.0);

    float gust = mix(1.0 - uWindGustAmount, 1.0, pow(0.5 + 0.5 * sin(t * uWindGustFreq), 3.0));
    vec3 dir = normalize(uWindDir);

    if (uWindMode == 0) {
        float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
        float h = clamp((uWindMaskMaxY - p.y) / denom, 0.0, 1.0);
        float mask = h * h; // more sway toward the hem
        float ph = t * uWindSpeed + p.y * 2.0 + p.x * 1.3;
        float sway = 0.5 + 0.5 * sin(ph); // 0..1 forward billow, never past rest
        float amp = uWindStrength * uWindResponse * mask * gust;
        vec3 flutter = vec3(sin(ph * 3.1), 0.0, cos(ph * 2.7)) * (uWindTurbulence * amp * 0.3);
        return dir * (sway * amp) + flutter;
    }

    // The trunk leans as one body, weighted by height squared so the base stays
    // planted, and every branch adds its own de-phased sway scaled by flex --
    // twigs travel far, limbs barely at all.
    float phase = uv1.x * 6.2831853;
    float flex = uv1.y;
    float amp = uWindStrength * uWindResponse * gust;

    float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
    float h = clamp((p.y - uWindMaskMinY) / denom, 0.0, 1.0);
    vec3 off = dir * ((0.5 + 0.5 * sin(t * uWindSpeed * 0.35)) * amp * h * h * 0.6);

    off += dir * (sin(t * uWindSpeed + phase) * amp * flex * 0.5);
    off += vec3(sin(t * uWindSpeed * 1.7 + phase * 2.0), 0.0,
                cos(t * uWindSpeed * 1.3 + phase)) *
           (uWindTurbulence * amp * flex * 0.25);

    if (uWindMode == 2) {
        // Leaf flutter rides on top of that: fast, weighted by UV0.y so the
        // card pivots about its stem (v=0) rather than sliding bodily, and
        // decorrelated by position so no two cards in a cluster beat in unison.
        float f = sin(t * uWindSpeed * 6.0 + phase * 7.0 + p.x * 3.0 + p.z * 2.7);
        off += vec3(f, f * 0.4, -f * 0.6) * (amp * flex * uv0.y * uWindTurbulence);
    }
    return off;
}

void main() {

    // Wind displaces the object-space position; the previous-frame position uses
    // t - dt so the motion vector stays honest (no TAA/motion-blur smear).
    vec3 posCurr = aPos + windOffset(aPos, aTexCoords, aTexCoords2, time);
    vec3 posPrev = aPos + windOffset(aPos, aTexCoords, aTexCoords2, time - uDeltaTime);

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

    // TBN. The bitangent is derived rather than stored: aTangent.w carries its
    // handedness (+1, or -1 on mirrored UV islands), which is the only thing
    // the fragment stage ever took from an authored bitangent anyway.
    vec3 T = normalize(mat3(model) * aTangent.xyz);
    vec3 N = normalize(mat3(model) * aNormal);
    vec3 B = cross(N, T) * aTangent.w;
    TBN = mat3(T, B, N);


    


    gl_Position = clipPos;
}


