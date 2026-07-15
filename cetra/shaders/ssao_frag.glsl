#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Screen-space ambient occlusion from the resolved depth buffer: for each
// pixel, count how many hemisphere sample points around its reconstructed
// view-space position are hidden behind nearer geometry. The hemisphere is
// oriented by the scene pass's normals G-buffer (true shading normals,
// including normal maps); when that buffer is disabled the normal falls
// back to depth derivatives, which facet on curved surfaces.
uniform sampler2D depthTex;   // Full-res resolved scene depth
uniform sampler2D noiseTex;   // 4x4 random rotation vectors, tiled
uniform sampler2D normalsTex; // View-space normals (xyz); .a unused here
uniform int useNormalsTex;
uniform mat4 projection;
uniform mat4 invProjection;
uniform vec3 samples[24]; // Hemisphere kernel (+Z oriented), set once
uniform vec2 noiseScale;  // ssao resolution / 4
uniform float radius;     // Occlusion reach in view-space units
uniform float bias;       // Depth acne guard

const int KERNEL_SIZE = 24;

vec3 viewPosFromDepth(vec2 uv, float depth)
{
    vec4 ndc = vec4(vec3(uv, depth) * 2.0 - 1.0, 1.0);
    vec4 view = invProjection * ndc;
    return view.xyz / view.w;
}

// Analytic view-space Z from an NDC depth (cglm right-handed perspective)
float viewZFromNdcZ(float ndcZ)
{
    return -projection[3][2] / (projection[2][2] + ndcZ);
}

void main()
{
    float depth = texture(depthTex, TexCoords).r;
    if (depth >= 1.0) {
        // Sky / background: fully unoccluded
        FragColor = vec4(1.0);
        return;
    }

    vec3 fragPos = viewPosFromDepth(TexCoords, depth);
    vec3 N;
    vec3 gbufferN = texture(normalsTex, TexCoords).xyz;
    // A2C draws (hair) skip the G-buffer, leaving cleared zero texels;
    // fall back to derivatives there as well
    if (useNormalsTex == 1 && dot(gbufferN, gbufferN) > 0.01) {
        N = normalize(gbufferN);
    } else {
        N = normalize(cross(dFdx(fragPos), dFdy(fragPos)));
    }

    // Per-pixel random kernel rotation turns banding into high-frequency
    // noise the box blur removes
    vec3 rvec = vec3(texture(noiseTex, TexCoords * noiseScale).xy, 0.0);
    vec3 T = normalize(rvec - N * dot(rvec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; i++) {
        vec3 samplePos = fragPos + TBN * samples[i] * radius;

        vec4 clip = projection * vec4(samplePos, 1.0);
        vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;

        float sceneZ = viewZFromNdcZ(texture(depthTex, uv).r * 2.0 - 1.0);

        // Ignore occluders much nearer than the sample (silhouette halos)
        float rangeCheck = smoothstep(0.0, 1.0, radius / abs(fragPos.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    FragColor = vec4(vec3(1.0 - occlusion / float(KERNEL_SIZE)), 1.0);
}
