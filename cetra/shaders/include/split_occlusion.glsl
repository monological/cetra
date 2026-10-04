// What the split composite multiplies the frame by (spec 11.4): the ambient specular by its
// occlusion, everything else by plain AO. One statement with two readers -- the composite, which
// applies it, and the wet-ground SSR fold (spec 13.9), which takes back out the share of that
// specular a reflection replaces and so must take out what the composite put in. It used to take
// the UNOCCLUDED share, assuming wet ground lies under open sky; under an eave that was more than
// the frame held, and a wet sill went below zero (spec 13.20).
//
// Declares the inputs; the includer binds them, and declares nothing of its own by these names.

uniform sampler2D aoTex;      // AO chain: .r visibility, .gba encoded bent normal
uniform sampler2D specOccTex; // Reflection-lobe sums: .r visible, .g the lobe it is of
uniform sampler2D normalsTex; // View normal .xyz + the SSR marker .a
uniform sampler2D auxTex;     // .z = linear view-Z, .w = effective roughness
uniform vec2 aoRes;           // aoTex's own size: it is HALF the render res
uniform int aoActive;         // 0 = no AO this frame: the frame is put back unoccluded
uniform float aoStrength;

#include "ao_upsample.glsl"
#include "spec_occ.glsl"

// (the ambient specular's multiplier, everything else's) at `uv`.
//
// The pixel's own depth is a plain fetch at render res, where aux is 1:1, and the two half-res
// buffers beneath it are what need reconstructing. Both magnifications land on the SAME weights
// without being made to: the weight set is a pure function of (auxTex, uv, aoRes, zRef), which is
// identical in the two calls, so a shared-weight variant would buy correctness that is already
// free and cost a near-duplicate of the helper. What it does spend is the four aux taps, twice.
vec2 splitOcclusionAt(vec2 uv)
{
    if (aoActive == 0)
        return vec2(1.0);
    float zRef = texture(auxTex, uv).z;
    vec4 aoSample = aoFetchBilateral(aoTex, auxTex, uv, aoRes, zRef);
    vec2 specPair = aoFetchBilateral(specOccTex, auxTex, uv, aoRes, zRef).rg;
    float so = specOccSplitAt(uv, aoSample, specPair);
    return vec2(mix(1.0, so, aoStrength), mix(1.0, aoSample.r, aoStrength));
}
