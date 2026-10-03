// A coat of fur SHELLS (spec 13.17), both halves: the draw is repeated as layers with
// gl_InstanceID the layer, and layer k of N stands k/N of a strand's length off the skin, layer 0
// being the skin itself; the fragment keeps only the strands' cross-sections. A vertex stage
// defines FUR_VERTEX before including this, as skin.glsl's includers define SKIN_PREV_POSE.
//
// Gated on its own bit, as ltc.glsl gates ltcTex: an includer without fur keeps none of this,
// declarations included, so the varyings exist exactly when both stages carry them.
#include "pbr_features.glsl"
// hash13. Outside the gate: includes are expanded once, ahead of the preprocessor, so one inside
// a branch some stage drops would take the hashes from every later include in that stage.
#include "noise.glsl"

#if CETRA_HAS(PBR_FEAT_FUR)
uniform int furLayers; // shells over the skin; 0 is a material with no coat

#ifdef FUR_VERTEX
#define FUR_VARYING out
#else
#define FUR_VARYING in
#endif
FUR_VARYING vec3 FurRest;        // bind-pose position: the strands ride the skin, not the world
flat FUR_VARYING float FurLayer; // 0 for the skin itself, k/N for shell k of N
FUR_VARYING float FurLen;        // this vertex's share of the coat's length

#ifdef FUR_VERTEX
uniform float furLength; // metres from root to tip, before the vertex's own share of it
uniform vec3 furComb;    // object-space direction the coat lies toward
uniform float furLie;    // 0 = strands stand straight out, 1 = they lie along the comb

// Where a strand's point at height h stands off the skin under `bone`: out along the normal,
// bending over toward the comb -- flattened onto the bind surface, then carried by the bone -- as
// it rises. Called once per pose with that pose's bone, so the two cannot bend differently.
vec3 furShellOffset(mat3 bone, vec3 normal, float h, float share) {
    vec3 n = normalize(bone * normal);
    vec3 comb = bone * (furComb - normal * dot(normal, furComb));
    float cl = length(comb);
    vec3 c = cl > 1e-6 ? comb / cl : n;
    return normalize(mix(n, c, furLie * h)) * (furLength * share * h);
}

#else
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
    tone = 1.0;
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
    // No strand is shorter than nothing, so none is wider here than one of full length: past
    // that radius this texel is bare whatever the strand's own length, and the tufts need not
    // be asked.
    if (best >= furThickness * mix(1.0, FUR_TIP, h))
        return false;
    float own = mix(0.45, 1.0, hash13(owner, vec3(26.651, 64.553, 17.849)));
    float tuft = mix(1.0, 0.5 + 0.7 * furTufts(owner / FUR_TUFT), furClump);
    float len = min(own * tuft, 1.0);
    if (h > len || best >= furThickness * mix(1.0, FUR_TIP, h / len))
        return false;
    tone = mix(0.85, 1.1, hash13(owner, vec3(91.733, 23.471, 58.117)));
    return true;
}
#endif
#endif
