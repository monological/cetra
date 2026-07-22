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
out vec2 TexCoords;
out vec2 TexCoords2;   // UV1 for lightmaps/AO
out vec4 VertexColor;  // Vertex color (RGBA)
out mat3 TBN;
flat out float TangentW; // bitangent handedness, per-island constant
out vec4 CurrClip;     // Un-jittered current clip position (motion vectors)
out vec4 PrevClip;     // Previous-frame clip position

// No Light struct / lights[] here on purpose: this stage does no lighting. It
// used to carry a copy declaring MAX_LIGHTS 70 while pbr_frag declares 64 --
// the same-named uniform array at two sizes in one linked program, which is a
// link error by spec and only linked because the vertex copy was inactive and
// stripped. pbr_frag owns that declaration (mirrored by PBR_MAX_LIGHTS in
// common.h); the moment a vertex shader needs lighting, include it once.

uniform mat4 model;
// transpose(inverse(model)), uploaded per node (render.c). Normals need it
// rather than `model` under non-uniform scale; computing it here would be a
// full mat4 inverse per vertex for a value constant across the draw.
uniform mat3 uNormalMatrix;
uniform mat4 view;
uniform mat4 projection; // Jittered when TAA is on (rasterization only)

// Motion-vector inputs (un-jittered). Default to zero when unset, yielding zero
// velocity, which the TAA resolve treats as static.
uniform mat4 uCurrViewProjNoJitter;
uniform mat4 uPrevViewProj;
uniform mat4 uPrevModel;

uniform float time;
uniform float uDeltaTime; // render clock advance, for the previous-frame position

#include "wind.glsl"
#include "tbn.glsl"

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

    Normal = normalize(uNormalMatrix * aNormal);
    TexCoords = aTexCoords;
    TexCoords2 = aTexCoords2;
    VertexColor = aColor;

    // TBN carries the SAME normal as the Normal varying: the fragment stage
    // orthogonalizes the basis against that varying, so a second, differently
    // transformed normal here would tilt the basis relative to the shading
    // normal under non-uniform scale.
    TBN = buildTBN(Normal, mat3(model) * aTangent.xyz, aTangent.w);
    TangentW = aTangent.w;

    gl_Position = clipPos;
}
