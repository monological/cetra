#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scattered radiance, a = extinction sigma

// Froxel fog, pass 1 of 3 (spec 9.5): evaluate the participating medium once
// per volume cell. The screen-space march this replaces evaluated the same
// lighting once per pixel per step, which made the clustered light list far too
// expensive to consult; a froxel pays for it once per cell.
//
// One draw per slice writes one layer of the volume (sliceIndex says which), so
// this is an ordinary fullscreen pass over the volume's XY grid.
//
// The cascade tap, spot in-scatter and Henyey-Greenstein phase mirror
// fog_frag.glsl; the height sigma deliberately does NOT (see below -- a volume
// cannot express the march's floor-plane ray clamp). A pixel has one view ray,
// so the march could hoist phase and the light-space projection out of its step
// loop; a froxel has no single ray, so both are evaluated per cell.

// Same mirrored constants as fog_frag.glsl (MAX_SHADOW_LIGHTS / SHADOW_CASCADES
// under private names); they must track shadow.h the same way.
#define MAX_FOG_LIGHTS 3
#define FOG_CASCADES 3

uniform int sliceIndex;  // Which volume layer this draw is writing
uniform int froxelDepth; // Slice count; mirrors POSTFX_FROXEL_Z
uniform mat4 projection; // Focal terms reconstruct the froxel's view position
uniform mat4 invView;    // view -> world (camera pose)
uniform float fogFar;    // Far end of the volume's exponential depth range

uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_FOG_LIGHTS * FOG_CASCADES];
uniform int cascadeCount;
uniform vec3 lightColor[MAX_FOG_LIGHTS]; // color * intensity
uniform vec3 lightDir[MAX_FOG_LIGHTS];   // normalized travel direction
uniform int numLights;
uniform vec3 ambientColor;
uniform float density;       // Extinction at floor height (1/world units)
uniform float heightFalloff; // World units for a 1/e density drop
uniform float floorY;        // World height of max density
uniform float anisotropy;    // Henyey-Greenstein g
uniform float sunBoost;
uniform float shadowBias;

// One volumetric spot light (the flashlight), as in fog_frag.
uniform int spotEnabled;
uniform vec3 spotPos;
uniform vec3 spotDir;
uniform vec3 spotColor;
uniform vec3 spotAtten;
uniform float spotCosInner;
uniform float spotCosOuter;
uniform int spotShadowed;
uniform sampler2D spotShadowMap;
uniform mat4 spotLightSpaceMatrix;

// Temporal reprojection against the previous frame's volume. 0 freezes the
// jitter and skips the blend, so headless renders stay byte-deterministic --
// the same contract every other accumulator in this stack honours.
uniform int temporal;
uniform int frameIndex;
uniform sampler3D historyVolume;
uniform mat4 prevView;       // World -> the previous frame's view space
uniform mat4 prevProjection; // Its focal terms map that to the previous volume

const float PI = 3.14159265359;

// Clustered light data for the local-light scattering below.
#include "lights_ubo.glsl"
#include "froxel.glsl"

// Normalized Henyey-Greenstein; c = cos(angle between light travel and the
// direction toward the camera)
float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Is the air at world position P lit by the caster whose cascade block starts
// at layer0? Walks cascades in index order and taps the FIRST whose box
// contains the point -- the tightest covering cascade. Outside every cascade
// counts as lit. Same rule as fog_frag's fogVisibility, but evaluated from a
// world position instead of a ray parameter (a froxel has no ray to be affine
// along, so the projection cannot be hoisted).
float fogVisibility(int layer0, vec3 P) {
    for (int c = 0; c < cascadeCount; c++) {
        int layer = layer0 + c;
        vec3 proj = (lightSpaceMatrix[layer] * vec4(P, 1.0)).xyz * 0.5 + 0.5;
        if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0) {
            continue;
        }
        float d = texture(shadowMaps, vec3(proj.xy, float(layer))).r;
        return proj.z - shadowBias > d ? 0.0 : 1.0;
    }
    return 1.0;
}

void main() {
    // Near plane recovered from the projection (the app's near clip varies) --
    // the same recovery contact_shadow_frag makes.
    float nearZ = projection[3][2] / (projection[2][2] - 1.0);
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);

    // Sample position within the cell. Frozen at the centre without temporal so
    // headless is deterministic; under TAA it walks the cell over frames and
    // the reprojected blend below averages the results, which is what hides the
    // volume's low resolution.
    float jitter = 0.5;
    if (temporal == 1) {
        jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                          0.00583715 * gl_FragCoord.y) +
                       float(frameIndex) * 0.61803398875);
    }
    vec3 viewPos =
        froxelViewPos(TexCoords, float(sliceIndex), jitter, nearZ, fogFar, float(froxelDepth),
                      invFocal);
    vec3 camPos = invView[3].xyz;
    vec3 P = (invView * vec4(viewPos, 1.0)).xyz;
    // Direction from the camera toward this cell: the phase function's second
    // argument, and the froxel equivalent of the march's per-pixel rayDir.
    vec3 rayDir = normalize(P - camPos);

    // Exponential height falloff above the floor. Below it the medium does not
    // exist at all: the screen-space march expressed that by clamping downward
    // rays at the floor plane, which a volume cannot do, so the equivalent is a
    // zero extinction there -- otherwise sub-floor cells would sit at the
    // formula's maximum density and fog the ground from underneath.
    float sigma = P.y < floorY ? 0.0 : density * exp(-(P.y - floorY) / heightFalloff);

    vec3 S = ambientColor;
    for (int j = 0; j < numLights; j++) {
        float phase = phaseHG(dot(lightDir[j], -rayDir), anisotropy) * sunBoost;
        S += lightColor[j] * (phase * fogVisibility(j * cascadeCount, P));
    }

    // Spot in-scatter at P: inside the cone, falling off with distance, cut by
    // the spot's shadow.
    if (spotEnabled == 1) {
        vec3 toL = spotPos - P;
        float d = length(toL);
        // spotConeFactor's own ramp, so shaft and floor pool share one formula.
        float cone = spotConeFactor(2.0, spotDir, spotCosInner, spotCosOuter, toL / max(d, 1e-4));
        if (cone > 0.0) {
            float atten = 1.0 / max(spotAtten.x + spotAtten.y * d + spotAtten.z * d * d, 1e-4);
            float vis = 1.0;
            if (spotShadowed == 1) {
                vec4 ls = spotLightSpaceMatrix * vec4(P, 1.0);
                if (ls.w > 0.0) { // in front of the light plane
                    vec3 pc = ls.xyz / ls.w * 0.5 + 0.5;
                    if (pc.z <= 1.0 && pc.x >= 0.0 && pc.x <= 1.0 && pc.y >= 0.0 && pc.y <= 1.0)
                        vis = (pc.z - shadowBias > texture(spotShadowMap, pc.xy).r) ? 0.0 : 1.0;
                }
            }
            float spotPhase = phaseHG(dot(spotDir, -rayDir), anisotropy) * sunBoost;
            S += spotColor * (cone * atten * spotPhase * vis);
        }
    }

    // Clustered point lights (spec 9.1's list), unshadowed -- the engine has no
    // shadow map for point lights. Attenuation matches pbr_frag so a light's
    // glow in the air agrees with the pool it casts on the floor.
    // No enable flag: the UBOs are zero-initialised and always bound, so a
    // scene without clustered lights reports count 0 and this costs nothing --
    // the degradation to sun+spot coverage is structural, not a toggle.
    uvec2 list = clusterLightListUv(TexCoords, -viewPos.z);
    for (uint k = 0u; k < list.y; k++) {
        uint li = lightIndexAt(list.x + k);
        // Point lights only. Area panels do not scatter in v1, and every spot
        // is skipped because the scene's first spot is already scattered above
        // WITH its shadow map and also appears in this list -- adding it again
        // would double it. Further spots go unscattered, as in the march.
        if (clusterLights[li].dirType.w != 1.0)
            continue;
        vec3 toL = clusterLights[li].posRange.xyz - P;
        float d = length(toL);
        float atten =
            1.0 / max(clusterLights[li].attenCutoff.x + clusterLights[li].attenCutoff.y * d +
                          clusterLights[li].attenCutoff.z * d * d,
                      1e-4);
        // toL points at the light, so -toL/d is the direction it travels -- the
        // punctual analogue of the directional phase above.
        float phase = phaseHG(dot(-toL / max(d, 1e-4), -rayDir), anisotropy) * sunBoost;
        S += clusterLights[li].colorIntensity.xyz * (atten * phase);
    }

    // Keep shafts HDR (they must bloom) but bound hostile parameter combos away
    // from fp16 overflow, as the screen-space march does.
    vec4 result = vec4(min(S, vec3(500.0)), sigma);

    // Temporal reprojection: find where this cell's world position sat in the
    // previous frame's volume and blend against it. Unlike the screen-space
    // passes there is no velocity buffer to reproject by -- a froxel is a
    // volume of air, not a surface -- so the previous camera does the mapping.
    if (temporal == 1) {
        vec4 prevViewPos = prevView * vec4(P, 1.0);
        float prevZ = -prevViewPos.z;
        if (prevZ > nearZ) {
            vec2 prevFocal = vec2(prevProjection[0][0], prevProjection[1][1]);
            vec2 prevUv = (prevViewPos.xy * prevFocal / prevZ) * 0.5 + 0.5;
            float prevSlice = froxelViewZToSlice(prevZ, nearZ, fogFar, float(froxelDepth));
            // Scatter cells sit at their slice centre (slice s spans continuous
            // s..s+1), so continuous coordinate c reads texel c-0.5, i.e. the
            // normalized coordinate is just c/depth.
            vec3 prevUvw = vec3(prevUv, prevSlice / float(froxelDepth));
            // Off-volume reprojection has no history to blend, so those cells
            // keep the current frame -- the standard disocclusion fallback.
            if (all(greaterThanEqual(prevUvw, vec3(0.0))) &&
                all(lessThanEqual(prevUvw, vec3(1.0)))) {
                vec4 history = texture(historyVolume, prevUvw);
                result = mix(result, history, 0.9);
            }
        }
    }

    FragColor = result;
}
