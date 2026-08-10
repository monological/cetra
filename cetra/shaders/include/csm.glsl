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

// The moment reconstruction, for the msmEnabled branch below.
#include "msm.glsl"

// Same declaration, two possible textures. Under --msm the C side binds an
// RGBA16F moment array to this sampler INSTEAD of the depth array, so .r stops
// being a depth and becomes the first moment -- which is why moment shadows cost
// no additional sampler, and why every read of this texture has to go through
// the branch rather than sampling .r directly.
uniform sampler2DArray shadowMaps;
uniform int msmEnabled;  // 1 = shadowMaps holds moments, not depths
uniform float msmBleed;  // Occlusion below this fraction is remapped to zero
// Layers stride by the RUNTIME cascadeCount (layer = slot*cascadeCount + c),
// so at cascadeCount 1 the indices match the classic single-map layout
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
// width, near, far; w unused (the vec4 stays: the C mirror is a cglm vec4
// array uploaded as one ranged glUniform4fv)
uniform vec4 cascadeParams[MAX_SHADOW_LIGHTS * SHADOW_CASCADES];
uniform int cascadeCount;
uniform vec2 shadowTexelSize;
// Translucent shadow maps (spec 11.26). Mirrors of shadow.h's layer law: the
// transmittance block sits past the cascade block in this SAME array, which is
// what lets one sampler carry both -- the transmittance has to be read
// ALONGSIDE the occlusion, so unlike the moments it cannot ride unit 10 by
// exclusion. TSM_SLOTS is 1: only the first directional caster has one.
#define TSM_PARTS 2
#define TSM_SLOTS 1
uniform int tsmEnabled; // 1 = the transmittance layers were built THIS frame
// Flat bias for the outermost-map consumers below; the per-fragment cascade
// path in pbr_frag uses the receiver-plane bias instead and never reads this.
uniform float shadowBias;

// Occlusion at one shadow-array layer from a SINGLE moment tap, 0 lit to 1 fully
// shadowed. Meaningful only where msmEnabled is 1; the two lookups below each
// branch to it rather than calling it unconditionally, because the depth path
// needs a kernel and a bias that this one has no use for -- the moments already
// carry the depth spread inside the footprint that those exist to cope with.
float csmMomentOcclusion(int layer, vec2 uv, float depth01) {
    return msmReduceBleed(
        msmOcclusion(texture(shadowMaps, vec3(uv, float(layer))), msmWarpDepth(depth01)), msmBleed);
}

// Light transmittance through the translucent casters in front of a receiver,
// 1 = nothing blocks. Multiplies the occlusion term rather than joining it: the
// two caster sets are disjoint by construction (a mesh goes to one map or the
// other), so they are independent attenuations of the same ray.
//
// The 3x3 box is exact here, which is the point of storing exp(-b0) rather than
// b0. Transmittance averages linearly; absorbance does not, and averaging it
// would carry Jensen's bias -- worst on exactly the dense casters this exists
// for. The moment path cannot make the same claim.
float csmTransmittance(int layer, vec2 uv, float depth01) {
    if (tsmEnabled == 0)
        return 1.0;
    int slot = layer / cascadeCount;
    if (slot >= TSM_SLOTS)
        return 1.0;
    int cascade = layer - slot * cascadeCount;
    int base = MAX_SHADOW_LIGHTS * cascadeCount + cascade * TSM_PARTS;

    // Front-layer gate. Without it every receiver is treated as behind every
    // translucent caster, so a shoulder IN FRONT of hair that hangs behind it
    // is wrongly darkened. The stored depth carries no polygon offset (the
    // transmittance passes run after it is disabled), hence the explicit bias.
    float nearest = texture(shadowMaps, vec3(uv, float(base + 1))).r;
    if (depth01 <= nearest + shadowBias)
        return 1.0;

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y)) * shadowTexelSize;
            sum += texture(shadowMaps, vec3(uv + o, float(base))).r;
        }
    }
    return sum / 9.0;
}

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

    // The moment map is already prefiltered, so the kernel below would be
    // blurring a blur -- and these consumers ask for softness, which is exactly
    // what the resolve's own blur radius sets.
    if (msmEnabled == 1)
        return csmMomentOcclusion(layer, proj.xy, proj.z);

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
    float occlusion = shadow / taps;

    // Translucent casters are withheld from the depth this function reads, so
    // without this they would be invisible to the catcher and the particles --
    // a glass panel that cast a solid shadow on the catcher ground would cast
    // NONE, which is worse than the feature being off. Polarity flips here:
    // this function returns occlusion where csmTransmittance returns
    // visibility.
    return 1.0 - (1.0 - occlusion) * csmTransmittance(layer, proj.xy, proj.z);
}

#endif // CSM_OUTERMOST_PCF
