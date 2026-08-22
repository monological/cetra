#version 330 core

/*
 * The composite-cache bake (spec 11.66). One fullscreen pass over a material's
 * splat domain, writing the layered surface's MACRO -- the splat weights and
 * the height blend, with every layer at its tile mean -- into the two cache
 * targets the shading path samples instead of running the blend per pixel.
 *
 * It calls sampleLayeredSurface UNCHANGED, which is the point: the cache is a
 * caching of the existing path, not a second implementation of it, so a change
 * to the blend cannot drift between the two. Two consequences fall out of the
 * pass's shape rather than from code here:
 *
 *   - dFdx(worldPos) IS the atlas texel size, so the splat tap inside
 *     layerWeights reads the splat at exactly the mip that averages one atlas
 *     texel's footprint.
 *   - layerGrainFrozen drives every LAYER tap to its top mip -- its mean -- so
 *     the macro carries no tile grain at any atlas resolution. The runtime
 *     detail term divides by the same per-layer mean, so on a flat map the
 *     ratio is exactly one and the macro passes through byte-exact.
 *
 * The bake runs in AUTHORED coordinates with uWorldOrigin left at zero, and at
 * y = 0 with a +Y normal: the macro's content -- weights and blend -- does not
 * depend on either, and grain, which does, is the runtime detail term's job
 * with the real position and normal.
 */

#include "layers.glsl"

uniform sampler2DArray materialArray;

// The world rect the current viewport covers: origin.xy, size.zw. The full
// fallback bake uploads splatDomain here; a PAGE bake uploads its own tile's
// rect (gutter included), and the viewport confines the draw -- so
// dFdx(worldPos) is one texel of whichever target is being written, which is
// what keeps the splat tap footprint-filtered in both cases.
uniform vec4 vtBakeRect;

in vec2 TexCoords;

// rgb = blended albedo in STORED codes, a = dominant layer index over 3 --
// quantised so texelFetch recovers it exactly with round(a * 3).
layout(location = 0) out vec4 OutAlbedo;
// rg = tangent normal xy in the ground's XZ frame, b = roughness, a = AO.
layout(location = 1) out vec4 OutSurface;

void main() {
    vec3 worldPos = vec3(vtBakeRect.x + TexCoords.x * vtBakeRect.z, 0.0,
                         vtBakeRect.y + TexCoords.y * vtBakeRect.w);
    LayerSurface s =
        sampleLayeredSurface(materialArray, worldPos, vec3(0.0, 1.0, 0.0), vec2(0.0));

    OutAlbedo = vec4(s.albedo, s.dominant / 3.0);
    // Baked against +Y, so the world normal's XZ components ARE the tangent
    // normal's xy; the runtime reconstructs z and composes onto the geometric
    // normal, which is what lets one bake serve every slope.
    OutSurface = vec4(s.normal.xz * 0.5 + 0.5, s.roughness, s.ao);
}
