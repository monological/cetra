// Clustered specular probes (spec 11.70): N parallax-corrected probes blended
// per fragment, selected by a per-froxel mask.
//
// The sampler is a PARAMETER rather than a declaration in here, which GLSL 330
// allows and which is what lets the lit surface and SSR share one
// implementation: pbr_frag passes giAtlasTex (the probe atlas is a tenant of
// the GI volume's texture, because pbr_frag declares sixteen samplers and the
// driver counts declarations), and ssr_frag passes its own. Two programs, one
// weight formula, nothing to drift.
//
// Everything about WHERE a probe's radiance lives is here and in probe_atlas.c.
// The rest of the engine knows only that probes exist.

layout(std140) uniform ProbeBlock {
    ivec4 probeInfo;      // x count (0 = disarmed), y mask bits set, zw unused
    vec4 probeAtlasParams; // 1/w, 1/h, w, h
    // [4i + 0] xyz capture position, w intensity
    // [4i + 1] xyz box min,          w box fade
    // [4i + 2] xyz box max,          w last row index
    // [4i + 3] x u0, y v0 (texels),  z row-0 tile size, w gutter
    vec4 probeDesc[8 * 4];
    // One 8-bit mask per froxel, four to a word. std140 gives a scalar array a
    // vec4 stride, so a uint[3072] would be four times this block.
    uvec4 probeClusterMasks[192];
};

#include "octahedral.glsl"

// Which probes reach this froxel. The decode mirrors clusterWord's in
// lights_ubo.glsl and for the same reason: the packing is what keeps the block
// small enough to sit beside the light blocks.
uint probeMaskAt(uint ci) {
    return (probeClusterMasks[ci >> 4u][(ci >> 2u) & 3u] >> ((ci & 3u) * 8u)) & 0xFFu;
}

// A fragment's mask, addressed through the light grid's own froxel lookup.
//
// Guarded because the mask is an OPTIMIZATION, not the answer: what decides a
// probe's contribution is its box weight, which is zero outside the box either
// way. A consumer that has lights_ubo.glsl (the lit surface) skips the probes
// the grid already rejected; one that does not (SSR, a post pass that would
// otherwise pull four light blocks in to save at most seven box tests) passes
// PROBE_MASK_ALL and lets the weights do it.
#ifdef PROBE_MASK_FROM_CLUSTERS
uint probeMask(vec2 fragCoord, float viewZ) {
    return probeMaskAt(clusterIndex(min(int(fragCoord.x * clusterParams.z), CLUSTER_X - 1),
                                    min(int(fragCoord.y * clusterParams.w), CLUSTER_Y - 1),
                                    viewZ));
}
#endif

#define PROBE_MASK_ALL 0xFFu

// How much of this fragment belongs to probe i: 1 anywhere inside its proxy
// box, falling to 0 over the probe's own fade measured OUTWARD from the faces.
//
// Outward, and that is a correctness requirement rather than a preference. A
// floor lies exactly ON the bottom face of the box that box-projects it -- the
// single-probe SSR path says so in as many words -- so an inward fade gives
// every floor in every scene a weight of exactly zero and hands the one surface
// this feature exists for back to the global environment. The blend therefore
// happens in the region where boxes OVERLAP or where their fades reach past
// each other, which is what an authored doorway is.
float probeBoxWeight(int i, vec3 P) {
    vec3 boxMin = probeDesc[4 * i + 1].xyz;
    vec3 boxMax = probeDesc[4 * i + 2].xyz;
    vec3 center = 0.5 * (boxMin + boxMax);
    vec3 halfExt = max(0.5 * (boxMax - boxMin), vec3(1e-4));
    vec3 dd = abs(P - center) / halfExt;
    float outer = max(dd.x, max(dd.y, dd.z));
    float fade = max(probeDesc[4 * i + 1].w, 1e-3);
    return clamp((1.0 + fade - outer) / fade, 0.0, 1.0);
}

// One probe's radiance along R at this roughness.
//
// The parallax correction is the single path's, term for term: intersect the
// world reflection ray with the proxy box, aim at the hit, and feather back to
// the plain direction near the faces where the correction degenerates.
vec3 probeRadiance(sampler2D atlas, int i, vec3 P, vec3 R, float rough) {
    vec3 boxMin = probeDesc[4 * i + 1].xyz;
    vec3 boxMax = probeDesc[4 * i + 2].xyz;
    vec3 probePos = probeDesc[4 * i + 0].xyz;

    vec3 invR = 1.0 / R;
    vec3 tMax3 = max((boxMax - P) * invR, (boxMin - P) * invR);
    float t = min(min(tMax3.x, tMax3.y), tMax3.z);
    vec3 corrected = (t > 0.0) ? normalize((P + R * t) - probePos) : R;

    vec3 center = 0.5 * (boxMin + boxMax);
    vec3 halfExt = max(0.5 * (boxMax - boxMin), vec3(1e-4));
    vec3 dd = abs(P - center) / halfExt;
    float fade = probeDesc[4 * i + 1].w;
    float inside = 1.0 - smoothstep(1.0 - fade, 1.0, max(dd.x, max(dd.y, dd.z)));
    vec3 dir = normalize(mix(R, corrected, inside));

    // Roughness picks a ROW, not a mip: the atlas has none, because generating
    // them would filter across the gutters that make an octahedral tile
    // bilinear-safe. Row r holds prefilter mip r, so this is the same
    // roughness->lod mapping the cube lookup uses, resolved by hand.
    float maxRow = probeDesc[4 * i + 2].w;
    float x = clamp(rough, 0.0, 1.0) * maxRow;
    float r0 = floor(x);
    float r1 = min(r0 + 1.0, maxRow);
    float f = x - r0;

    vec2 oct = octEncode(dir) * 0.5 + 0.5;
    vec4 rect = probeDesc[4 * i + 3];
    float row0 = rect.z;
    float gutter = rect.w;

    // Walk down the column to the row's origin. The rows halve until they stop
    // at the atlas's floor, and this loop is the shader's copy of that: eight
    // iterations of two adds, against a per-row uniform array that would cost a
    // descriptor row each.
    vec2 uvA = vec2(0.0), uvB = vec2(0.0);
    float y = 0.0;
    for (int r = 0; r <= 7; ++r) {
        float res = max(row0 / exp2(float(r)), 8.0);
        float pitch = res + 2.0 * gutter;
        if (float(r) == r0)
            uvA = (rect.xy + vec2(0.0, y) + gutter + oct * res) * probeAtlasParams.xy;
        if (float(r) == r1)
            uvB = (rect.xy + vec2(0.0, y) + gutter + oct * res) * probeAtlasParams.xy;
        y += pitch;
    }

    vec3 a = textureLod(atlas, uvA, 0.0).rgb;
    vec3 b = textureLod(atlas, uvB, 0.0).rgb;
    return mix(a, b, f) * probeDesc[4 * i + 0].w;
}

// The blend. Returns the probes' weighted radiance in rgb and the total weight
// in a, so the caller supplies its own environment fallback for whatever is
// left over -- which is what lets the fallback stay the caller's VERBATIM
// expression rather than a second copy living in here.
//
// Weights are normalized only when they exceed 1: inside overlapping boxes the
// probes share the fragment, and outside all of them the remainder falls to the
// environment rather than being manufactured out of a probe that does not
// reach.
vec4 probeSetSpecular(sampler2D atlas, uint mask, vec3 P, vec3 R, float rough) {
    vec3 sum = vec3(0.0);
    float total = 0.0;

    for (int i = 0; i < 8; ++i) {
        if (i >= probeInfo.x)
            break;
        if ((mask & (1u << uint(i))) == 0u)
            continue;
        float w = probeBoxWeight(i, P);
        if (w <= 0.0)
            continue;
        sum += w * probeRadiance(atlas, i, P, R, rough);
        total += w;
    }

    if (total <= 0.0)
        return vec4(0.0);
    return vec4(sum / max(total, 1.0), min(total, 1.0));
}
