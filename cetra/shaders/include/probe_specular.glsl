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
    ivec4 probeInfo;       // x count (0 = disarmed), yzw unused
    vec4 probeAtlasParams; // 1/w, 1/h, w, h
    // Atlas-wide, not per probe: xy unused, z gutter, w last row index.
    vec4 probeAtlasColumn;
    // Row r's y origin (texels, gutter included) in .x and its interior edge in
    // .y. ATLAS-wide for the same reason -- every probe's column has identical
    // row geometry, so this is a table and not eight copies of one.
    vec4 probeRow[8];
    // [4i + 0] xyz capture position, w intensity
    // [4i + 1] xyz box min,          w box fade
    // [4i + 2] xyz box max,          w unused
    // [4i + 3] x column left edge (texels), yzw unused
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

// "Every probe", for a consumer with no froxel to look one up by.
//
// The mask is an OPTIMIZATION, not the answer: what decides a probe's
// contribution is its box weight, which is zero outside the box either way. The
// lit surface has lights_ubo.glsl and passes the real mask to skip probes the
// grid already rejected; SSR is a post pass that would have to pull four light
// blocks in to save at most seven box tests, so it passes this and lets the
// weights do it.
#define PROBE_MASK_ALL 0xFFu

// Must match PROBE_SET_MAX (probe_set.h). Held by the driver's own std140 size
// check against UBO_PROBES_BLOCK_SIZE, which probeDesc's declaration rides --
// the same way lights_ubo.glsl's MAX_CLUSTER_LIGHTS is held.
const int PROBE_SET_MAX = 8;

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
// How far outside probe i's proxy box this fragment is, normalized so 0 is the
// centre and 1 is the faces. Computed ONCE per probe and handed to both the
// weight and the parallax feather below -- they apply different curves to it
// (an outward ramp and an inward smoothstep) but they must agree on the
// distance, and computing it twice is how they would stop agreeing.
float probeBoxOuter(int i, vec3 P) {
    vec3 boxMin = probeDesc[4 * i + 1].xyz;
    vec3 boxMax = probeDesc[4 * i + 2].xyz;
    vec3 center = 0.5 * (boxMin + boxMax);
    vec3 halfExt = max(0.5 * (boxMax - boxMin), vec3(1e-4));
    vec3 dd = abs(P - center) / halfExt;
    return max(dd.x, max(dd.y, dd.z));
}

// The probe's fade, floored. ONE reader of the field, because a raw
// probeDesc[...].w reaches smoothstep as edge0 == edge1 at fade 0 -- undefined
// in GLSL, and 0 is directly reachable: the GUI slider spans [0, 0.5] and
// boxFade is a .cscn key.
float probeFade(int i) {
    return max(probeDesc[4 * i + 1].w, 1e-3);
}

float probeBoxWeight(float outer, float fade) {
    return clamp((1.0 + fade - outer) / fade, 0.0, 1.0);
}

/*
 * The parallax correction, and the ONE statement of it in the engine.
 *
 * Intersect the world reflection ray with the proxy box, aim at the hit, and
 * feather back to the plain direction near the faces where the correction
 * degenerates (it collapses toward the surface point there); a ray starting
 * outside the box keeps the uncorrected direction too.
 *
 * Takes plain vectors rather than a probe index so the single-probe path in
 * pbr_frag -- whose probe arrives in scalar uniforms, not in this block -- can
 * call it as well. It was written out three times before this: here, in
 * pbr_frag's probeEnabled branch and in ssr_frag, which is the drift the spec
 * claimed had been factored out and had not.
 *
 * `outer` is the normalized box distance the caller already has; `fade` must be
 * floored (probeFade) because smoothstep with edge0 == edge1 is undefined.
 */
vec3 probeParallaxDir(vec3 boxMin, vec3 boxMax, vec3 probePos, vec3 P, vec3 R, float outer,
                      float fade) {
    vec3 invR = 1.0 / R;
    vec3 tMax3 = max((boxMax - P) * invR, (boxMin - P) * invR);
    float t = min(min(tMax3.x, tMax3.y), tMax3.z);
    vec3 corrected = (t > 0.0) ? normalize((P + R * t) - probePos) : R;

    float inside = 1.0 - smoothstep(1.0 - fade, 1.0, outer);
    return normalize(mix(R, corrected, inside));
}

// One probe's radiance along R at this roughness.
vec3 probeRadiance(sampler2D atlas, int i, vec3 P, vec3 R, float rough, float outer) {
    vec3 dir = probeParallaxDir(probeDesc[4 * i + 1].xyz, probeDesc[4 * i + 2].xyz,
                                probeDesc[4 * i + 0].xyz, P, R, outer, probeFade(i));

    // Roughness picks a ROW, not a mip: the atlas has none, because generating
    // them would filter across the gutters that make an octahedral tile
    // bilinear-safe. Row r holds prefilter mip r, so this is the same
    // roughness->lod mapping the cube lookup uses, resolved by hand.
    float maxRow = probeAtlasColumn.w;
    float x = clamp(rough, 0.0, 1.0) * maxRow;
    int r0 = int(floor(x));
    int r1 = int(min(float(r0) + 1.0, maxRow));
    float f = x - float(r0);

    vec2 oct = octEncode(dir) * 0.5 + 0.5;
    // The column's left edge; the rows' y origins and sizes are ATLAS-wide, so
    // they are one table rather than per-probe state.
    float u0 = probeDesc[4 * i + 3].x;
    float gutter = probeAtlasColumn.z;

    // Two indexed reads, where this walked all eight rows re-deriving their
    // pitches to recover two of them. The rows are identical for every probe
    // and never change after the atlas is built, so the CPU publishes them --
    // which also removes the shader's copy of the halving rule, and with it the
    // requirement that the row size be a power of two (the C side shifts, and a
    // divide by exp2 only agrees with a shift on those).
    vec2 rowA = probeRow[r0].xy;
    vec2 rowB = probeRow[r1].xy;
    vec2 uvA = (vec2(u0, rowA.x) + gutter + oct * rowA.y) * probeAtlasParams.xy;
    vec2 uvB = (vec2(u0, rowB.x) + gutter + oct * rowB.y) * probeAtlasParams.xy;

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

    for (int i = 0; i < PROBE_SET_MAX; ++i) {
        if (i >= probeInfo.x)
            break;
        if ((mask & (1u << uint(i))) == 0u)
            continue;
        float outer = probeBoxOuter(i, P);
        float w = probeBoxWeight(outer, probeFade(i));
        if (w <= 0.0)
            continue;
        sum += w * probeRadiance(atlas, i, P, R, rough, outer);
        total += w;
    }

    if (total <= 0.0)
        return vec4(0.0);
    return vec4(sum / max(total, 1.0), min(total, 1.0));
}
