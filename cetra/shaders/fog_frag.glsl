#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Volumetric fog: march the view ray from the camera to the pixel's depth
// (a scene-scaled far distance for sky), accumulating single-scattered
// light where the shadow maps say the air is lit, plus an isotropic
// ambient term, through an exponential height-fog density. Outputs
// (inscatter.rgb, transmittance.a) at half res; a separate pass composites
// scene * transmittance + inscatter into the HDR target.

#define MAX_FOG_LIGHTS 3

uniform sampler2D linDepthTex;     // Full-res aux: .z = linear view-Z (0 = sky)
uniform sampler2DArray shadowMaps; // The scene's shadow map array
uniform mat4 invView;              // view -> world (camera pose)
uniform mat4 projection;           // Only the focal terms are used
uniform mat4 lightSpaceMatrix[MAX_FOG_LIGHTS];
uniform vec3 lightColor[MAX_FOG_LIGHTS]; // color * intensity
uniform vec3 lightDir[MAX_FOG_LIGHTS];   // normalized travel direction
uniform int numLights;
uniform vec3 ambientColor;
uniform float density;       // Extinction at floor height (1/world units)
uniform float heightFalloff; // World units for a 1/e density drop
uniform float floorY;        // World height of max density
uniform float fogFar;        // Sky-ray march length / march cap
uniform float anisotropy;    // Henyey-Greenstein g
uniform float sunBoost;
uniform float shadowBias;
uniform int steps;
uniform int temporal; // 1 when the temporal accumulator integrates frames
uniform int frameIndex;

const float PI = 3.14159265359;

// View-space position from screen UV + stored linear view Z (RH, z < 0) —
// the same reconstruction the AO pass uses
vec3 viewPosFromLinZ(vec2 uv, float linZ, vec2 invFocal)
{
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc * (-linZ) * invFocal, linZ);
}

// Normalized Henyey-Greenstein; c = cos(angle between light travel and the
// direction toward the camera)
float phaseHG(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// One shadow tap: is the air at shadow-map position `proj` lit by caster
// `slot`? Outside the shadow volume counts as lit (the ortho box only
// covers the scene's neighborhood).
float fogVisibility(int slot, vec3 proj)
{
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
        return 1.0;
    }
    float d = texture(shadowMaps, vec3(proj.xy, float(slot))).r;
    return proj.z - shadowBias > d ? 0.0 : 1.0;
}

void main()
{
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    float linZ = texture(linDepthTex, TexCoords).z;
    bool sky = (linZ >= -1e-4); // aux sentinel: sky/background writes 0

    vec3 camPos = invView[3].xyz;
    // For sky pixels reconstruct the ray direction at an arbitrary depth
    vec3 viewPos = viewPosFromLinZ(TexCoords, sky ? -1.0 : linZ, invFocal);
    vec3 worldPos = (invView * vec4(viewPos, 1.0)).xyz;
    vec3 rayDir = normalize(worldPos - camPos);

    float tEnd = sky ? fogFar : length(viewPos);
    tEnd = min(tEnd, fogFar);
    // Air below the fog floor doesn't exist: clamp downward rays at the
    // floor plane (also terminates dome-ground "sky" rays where a real
    // floor depth would)
    if (rayDir.y < -1e-5) {
        tEnd = min(tEnd, (floorY - camPos.y) / rayDir.y);
    }
    if (tEnd <= 0.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Deterministic per-pixel dither (interleaved gradient noise) breaks
    // step banding; rotated per frame only when the temporal accumulator
    // integrates the jittered results
    float jitter =
        fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
    if (temporal == 1) {
        jitter = fract(jitter + float(frameIndex) * 0.61803398875);
    }

    // Directional ortho casters make everything about a light constant or
    // affine along the ray: phase and color fold into one gain, and the
    // light-space projection (w == 1 under an ortho matrix, so no divide)
    // collapses to origin + t * direction — one fma per step instead of a
    // full matrix transform.
    vec3 lightK[MAX_FOG_LIGHTS];
    vec3 lsBase[MAX_FOG_LIGHTS];
    vec3 lsDelta[MAX_FOG_LIGHTS];
    for (int j = 0; j < numLights; j++) {
        lightK[j] = lightColor[j] * (phaseHG(dot(lightDir[j], -rayDir), anisotropy) * sunBoost);
        lsBase[j] = (lightSpaceMatrix[j] * vec4(camPos, 1.0)).xyz * 0.5 + 0.5;
        lsDelta[j] = (lightSpaceMatrix[j] * vec4(rayDir, 0.0)).xyz * 0.5;
    }

    float dt = tEnd / float(steps);
    vec3 L = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < steps; i++) {
        float t = (float(i) + jitter) * dt;
        vec3 P = camPos + rayDir * t;
        float sigma = density * exp(-max(P.y - floorY, 0.0) / heightFalloff);
        // Energy-conserving per-segment integration: the analytic integral
        // of S * sigma * exp(-sigma x) over the step, stable for any
        // sigma * dt (a plain Riemann sum overshoots at scene scale)
        float stepTrans = exp(-sigma * dt);
        vec3 S = ambientColor;
        for (int j = 0; j < numLights; j++) {
            S += lightK[j] * fogVisibility(j, lsBase[j] + t * lsDelta[j]);
        }
        L += T * (1.0 - stepTrans) * S;
        T *= stepTrans;
        if (T < 0.003) {
            T = 0.0;
            break;
        }
    }

    // Keep shafts HDR (they must bloom) but bound hostile parameter combos
    // away from fp16 overflow
    FragColor = vec4(min(L, vec3(500.0)), T);
}
