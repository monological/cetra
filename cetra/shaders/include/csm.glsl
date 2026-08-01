// Cascaded shadow map interface: the uniform block that shadow.c's
// bind_shadow_maps_to_program fills, shared by every fragment shader that
// reads the cascades (pbr_frag, catcher_frag, particle_frag).
//
// These names are a contract with ONE C function, and the binder is
// location-guarded: rename a uniform here and the upload silently no-ops in
// whichever shader you forgot, with no error anywhere. That is the failure
// this chunk exists to make impossible -- there is now one text to rename.
//
// The array sizes are mirrored in C at shadow.h (MAX_SHADOW_LIGHTS,
// SHADOW_CASCADES). uniform.c's warn_if_array_shorter runs an explicit drift
// check at program setup and warns if the shader arrays came out smaller than
// the C side believes, which would silently truncate the ranged uploads.
//
// NOT included by froxel_inject_frag, deliberately: it declares the same
// block under private names (MAX_FOG_LIGHTS / FOG_CASCADES), omits
// cascadeParams, and is fed by a separate upload path in postfx.c rather than
// by bind_shadow_maps_to_program. Unifying it means reconciling two upload
// paths and is the highest-risk, lowest-payoff move in this area.

#define MAX_SHADOW_LIGHTS 3
#define SHADOW_CASCADES 3

uniform sampler2DArray shadowMaps;
// Layers stride by the RUNTIME cascadeCount (layer = slot*cascadeCount + c),
// so at cascadeCount 1 the indices match the classic single-map layout
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
// width, near, far; w unused (the vec4 stays: the C mirror is a cglm vec4
// array uploaded as one ranged glUniform4fv)
uniform vec4 cascadeParams[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
uniform int cascadeCount;
uniform vec2 shadowTexelSize;
// Flat bias for the outermost-map consumers below; the per-fragment cascade
// path in pbr_frag uses the receiver-plane bias instead and never reads this.
uniform float shadowBias;

// Define CSM_OUTERMOST_PCF before including for the soft scene-scale lookup
// that the shadow catcher and the particle motes share, and set
// CSM_PCF_HALF_KERNEL to the kernel radius (1 -> 3x3, 2 -> 5x5). It is a
// compile-time define rather than a parameter so the loops keep constant
// bounds and unroll exactly as the hand-written copies did.
#ifdef CSM_OUTERMOST_PCF

#ifndef CSM_PCF_HALF_KERNEL
#define CSM_PCF_HALF_KERNEL 1
#endif

uniform int numShadowLights;

// Occlusion from the OUTERMOST cascade, which is the classic
// camera-independent scene-fit map (complete for every caster in the scene by
// construction). One map means no per-fragment selection, no seams, and no
// boundary that can move with the camera -- exactly the pre-cascade floor
// behaviour, and exactly right for volumetric motes. At cascadeCount 1 the
// layer is the classic slot index.
//
// Flat shadowBias on purpose: the receivers here are a virtual plane
// (catcher) or air (motes), never surfaces stored in the map, so a flat
// delay of occlusion onset is all a bias has to do. The kernel step is 1.5
// texels of the scene-fit map the constant was tuned against -- which is the
// map this function always samples.
float csmOutermostOcclusion(vec3 worldPos, int slot)
{
    int layer = slot * cascadeCount + (cascadeCount - 1);
    vec4 lightSpace = lightSpaceMatrix[layer] * vec4(worldPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;

    vec2 kernelStep = shadowTexelSize * 1.5;
    float shadow = 0.0;
    for (int x = -CSM_PCF_HALF_KERNEL; x <= CSM_PCF_HALF_KERNEL; x++) {
        for (int y = -CSM_PCF_HALF_KERNEL; y <= CSM_PCF_HALF_KERNEL; y++) {
            vec2 offset = vec2(float(x), float(y)) * kernelStep;
            float depth = texture(shadowMaps, vec3(proj.xy + offset, float(layer))).r;
            shadow += proj.z - shadowBias > depth ? 1.0 : 0.0;
        }
    }
    const float taps = float(2 * CSM_PCF_HALF_KERNEL + 1) * float(2 * CSM_PCF_HALF_KERNEL + 1);
    return shadow / taps;
}

#endif // CSM_OUTERMOST_PCF
