#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Split spec-occ composite (spec 11.4): the scene pass routed ambient
// specular to its own buffer, so occlusion can finally multiply exactly what
// it models -- the cone term on the specular share, plain AO on everything
// else -- instead of guessing a per-pixel specular fraction. One blended
// pass, the fog fold's idiom: this outputs (spec * SO, aoFactor) and the
// (GL_ONE, GL_SRC_ALPHA) blend forms spec * SO + scene * aoFactor in place.
// The tonemap's own AO share collapses to 1 in split mode; its
// contact-shadow fold stays (direct light, independent of ambient
// occlusion). Runs before the TAA resolve so the reunited frame is
// stabilized as one image.
//
// Wet ground's screen-space reflection lands here too (spec 13.21), and for the same reason. It
// REPLACES a share of the environment's reflection (see ssr_frag), and that share is in this
// buffer, so the pair is folded in as spec * SO * (1 - coverage) + reflection: every term
// non-negative. Subtracted after TAA instead, the share came out of a frame TAA had resolved
// and upscaled while what was taken was this frame's render-res value, and a ripple or a wet
// edge went below zero. The pair is the frame before's, the only trace there is yet, read where
// the surface was; a reflection moves with parallax rather than with its surface, which the SSR
// accumulator's reprojection already accepts.

uniform sampler2D specTex;    // Resolved ambient specular (working space), render res
uniform sampler2D aoTex;      // AO chain: .r visibility, .gba encoded bent normal
uniform sampler2D specOccTex; // Reflection-lobe sums: .r visible, .g the lobe it is of
uniform sampler2D normalsTex; // View normal .xyz + the SSR marker .a
uniform sampler2D auxTex;     // .xy velocity (UV), .z linear view-Z, .w roughness, render res
uniform sampler2D ssrPrevTex; // Last frame's SSR result; wet pairs are (reflection, coverage)
uniform vec2 aoRes;           // aoTex's own size: it is HALF the render res
uniform int aoActive;         // 0 = no AO this frame: the specular goes back unoccluded
uniform float aoStrength;
uniform int ssrPrevActive;    // 0 = no trace from the frame before: wet ground takes none
uniform float ssrPrevScale;   // that trace's radiance to this frame's pre-exposure

#include "ao_upsample.glsl"
#include "spec_occ.glsl"
#include "ssr_marker.glsl"

// (the ambient specular as it goes back, the multiplier on everything else) at `uv`.
vec4 splitOcclusionAt(vec2 uv)
{
    vec3 spec = texture(specTex, uv).rgb;
    if (aoActive == 0)
        return vec4(spec, 1.0);
    float zRef = texelFetch(auxTex, ivec2(uv * vec2(textureSize(auxTex, 0))), 0).z;
    vec4 aoSample = aoFetchBilateral(aoTex, auxTex, uv, aoRes, zRef);
    vec2 specPair = aoFetchBilateral(specOccTex, auxTex, uv, aoRes, zRef).rg;
    float so = specOccSplitAt(uv, aoSample, specPair);
    return vec4(spec * mix(1.0, so, aoStrength), mix(1.0, aoSample.r, aoStrength));
}

// `spec`, the ambient specular going back at `uv`, with the share a wet surface's reflection
// stands in for replaced by it. The read is bilinear and blind to class, so where wet ground meets
// the catcher it can take a catcher's pair, whose alpha is a Fresnel rather than a coverage; the
// late fold separates the classes and this does not.
//
// Off the edge of the frame before -- the strip a turn reveals at its leading edge -- wet ground
// takes the nearest of that frame's reflections (spec 13.46). Taking the environment's share
// alone there, what --no-ssr shows, flashes a strip at the leading edge of every turn wherever
// that share is brighter than what the trace sees, as on a wet street under a fogged night sky.
// The borrow fades out from WET_EDGE_REACH to twice it off the frame, so a cut takes the
// environment's share: one edge column lent to the whole frame lands further from the true
// reflection than that share does.
const float WET_EDGE_REACH = 0.1; // of the frame: several times a fast turn's strip a frame
vec3 wetReflection(vec2 uv, vec3 spec)
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (!ssrMarkerIsWet(texelFetch(normalsTex, px, 0).a))
        return spec;
    vec2 prevUv = uv - texelFetch(auxTex, px, 0).xy;
    vec2 nearest = clamp(prevUv, vec2(0.0), vec2(1.0));
    vec2 off = abs(prevUv - nearest);
    float lend = 1.0 - smoothstep(WET_EDGE_REACH, 2.0 * WET_EDGE_REACH, max(off.x, off.y));
    vec4 pair = texture(ssrPrevTex, nearest) * lend;
    return spec * (1.0 - pair.a) + pair.rgb * ssrPrevScale;
}

void main()
{
    vec4 composite = splitOcclusionAt(TexCoords);
    if (ssrPrevActive != 0)
        composite.rgb = wetReflection(TexCoords, composite.rgb);
    FragColor = composite;
}
