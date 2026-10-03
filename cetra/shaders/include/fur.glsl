// The fragment half of a coat of fur shells (spec 13.17): which texels of a shell are coat, and
// how a strand is shaded along its length. The vertex half is pbr_skinned_vert's, which stands
// each shell off the skin and hands over the layer and the bind-pose position.
//
// Gated on its own bit, as ltc.glsl gates ltcTex: an includer without fur keeps none of this,
// declarations included, so the varyings below exist exactly when a vertex stage writes them.
#include "pbr_features.glsl"

#if CETRA_HAS(PBR_FEAT_FUR)
in vec3 FurRest;        // bind-pose position: the strands ride the skin, not the world
flat in float FurLayer; // 0 for the skin itself, k/N for shell k of N
in float FurLen;        // this vertex's share of the coat's length

uniform float furDensity;   // strands per metre of skin
uniform float furThickness; // a strand's radius at its root, in strand spacings
uniform float furRootShade; // the albedo at a strand's root against 1 at its tip
uniform float furClump;     // 0 = every strand its own length, 1 = lengths gathered in tufts

// Below this share of the coat's length a vertex grows nothing: its shells would lie on the skin
// and z-fight it, which round the eyes and the mouth is where it is meant to be bare.
#define FUR_BARE 0.05
// How far a strand narrows by its tip, as a fraction of its root radius.
#define FUR_TIP 0.25
// Tufts are this many strands across.
#define FUR_TUFT 4.0

// Smooth value noise in [0,1): the tufts a coat gathers into.
float furTufts(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    const vec3 k = vec3(19.913, 47.719, 31.513);
    float a = mix(hash13(i, k), hash13(i + vec3(1, 0, 0), k), f.x);
    float b = mix(hash13(i + vec3(0, 1, 0), k), hash13(i + vec3(1, 1, 0), k), f.x);
    float c = mix(hash13(i + vec3(0, 0, 1), k), hash13(i + vec3(1, 0, 1), k), f.x);
    float d = mix(hash13(i + vec3(0, 1, 1), k), hash13(i + vec3(1, 1, 1), k), f.x);
    return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

// Whether this shell texel is coat. `p` is the bind-pose position in strand spacings: the nearest
// strand is the nearest of the feature points of the eight cells round it, and it is cut at its
// own length, which tufts gather, with a radius narrowing from root to tip. Past half a spacing at
// the root the strands overlap, so the lowest shells are an unbroken undercoat and the coat thins
// toward an uneven top -- a coat, where thin roots read as separate hairs over bare skin.
// Worley on the bind pose, so no texture, no sampler unit and nothing to unwrap.
bool furStrand(vec3 p, float h, out float tone) {
    vec3 base = floor(p - 0.5);
    float best = 1e9;
    vec3 owner = base;
    for (int i = 0; i < 8; i++) {
        vec3 cell = base + vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        vec3 point = cell + 0.15 + 0.7 * vec3(hash13(cell, vec3(12.9898, 78.233, 37.719)),
                                              hash13(cell, vec3(39.346, 11.135, 83.155)),
                                              hash13(cell, vec3(73.156, 52.235, 9.151)));
        float d = length(p - point);
        if (d < best) {
            best = d;
            owner = cell;
        }
    }
    float own = mix(0.45, 1.0, hash13(owner, vec3(26.651, 64.553, 17.849)));
    float tuft = mix(1.0, 0.5 + 0.7 * furTufts(owner / FUR_TUFT), furClump);
    float len = min(own * tuft, 1.0);
    tone = mix(0.85, 1.1, hash13(owner, vec3(91.733, 23.471, 58.117)));
    if (h > len)
        return false;
    return best < furThickness * mix(1.0, FUR_TIP, h / len);
}
#endif
