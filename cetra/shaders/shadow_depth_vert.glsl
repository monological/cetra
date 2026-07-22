#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoords;
layout (location = 6) in ivec4 aBoneIds;
layout (location = 7) in vec4 aBoneWeights;
layout (location = 8) in vec2 aTexCoords2;

#define MAX_BONES 128

out vec2 TexCoords; // for the alpha test on foliage (material.h foliage_shadows)

uniform mat4 model;
uniform mat4 lightSpaceMatrix;

// Skinning (mirrors pbr_skinned_vert so animated meshes cast animated
// shadows). The skinned flag gates the path because disabled vertex
// attributes read as (0,0,0,1), which would corrupt unskinned meshes.
uniform bool skinned;
uniform mat4 boneMatrices[MAX_BONES];

// Directional wind, mirroring pbr_vert.glsl so a swaying surface casts a
// swaying shadow -- if the depth pass skipped the displacement, the canopy
// would sway while its shadow on the ground stayed nailed in place. Zero
// unless the scene has wind and this material opted in, so wind-free scenes
// render exactly as before.
uniform float time;
uniform vec3 uWindDir;
uniform float uWindStrength;
uniform float uWindSpeed;
uniform float uWindGustFreq;
uniform float uWindGustAmount;
uniform float uWindTurbulence;
uniform float uWindResponse;
uniform float uWindMaskMinY;
uniform float uWindMaskMaxY;
uniform int uWindMode;

vec3 windOffset(vec3 p, vec2 uv0, vec2 uv1, float t) {
    if (uWindStrength <= 0.0 || uWindResponse <= 0.0)
        return vec3(0.0);

    float gust = mix(1.0 - uWindGustAmount, 1.0, pow(0.5 + 0.5 * sin(t * uWindGustFreq), 3.0));
    vec3 dir = normalize(uWindDir);

    if (uWindMode == 0) {
        float denom = max(uWindMaskMaxY - uWindMaskMinY, 1e-4);
        float h = clamp((uWindMaskMaxY - p.y) / denom, 0.0, 1.0);
        float mask = h * h;
        float ph = t * uWindSpeed + p.y * 2.0 + p.x * 1.3;
        float sway = 0.5 + 0.5 * sin(ph);
        float amp = uWindStrength * uWindResponse * mask * gust;
        vec3 flutter = vec3(sin(ph * 3.1), 0.0, cos(ph * 2.7)) * (uWindTurbulence * amp * 0.3);
        return dir * (sway * amp) + flutter;
    }

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
        float f = sin(t * uWindSpeed * 6.0 + phase * 7.0 + p.x * 3.0 + p.z * 2.7);
        off += vec3(f, f * 0.4, -f * 0.6) * (amp * flex * uv0.y * uWindTurbulence);
    }
    return off;
}

void main()
{
    vec4 localPos = vec4(aPos, 1.0);

    if (skinned) {
        mat4 boneTransform = mat4(0.0);
        float totalWeight = 0.0;

        for (int i = 0; i < 4; i++) {
            if (aBoneIds[i] >= 0 && aBoneIds[i] < MAX_BONES) {
                boneTransform += boneMatrices[aBoneIds[i]] * aBoneWeights[i];
                totalWeight += aBoneWeights[i];
            }
        }

        if (totalWeight >= 0.001) {
            localPos = boneTransform * vec4(aPos, 1.0);
        }
    }

    localPos.xyz += windOffset(aPos, aTexCoords, aTexCoords2, time);

    TexCoords = aTexCoords;
    gl_Position = lightSpaceMatrix * model * localPos;
}
