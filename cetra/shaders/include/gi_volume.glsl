// DDGI irradiance probe volume sampling (spec 9.7).
//
// Replaces the single direction-only `texture(irradianceMap, N)` -- which is
// identical at every point in the scene and so can carry no bounce, no colour
// bleed and no falloff away from an opening -- with an 8-probe trilinear read
// from a grid of probes that each saw the real scene.
//
// Two guards keep light on the correct side of geometry, and both are needed:
//
//   BACKFACE. A probe sitting behind the shaded surface saw the far side of the
//   wall. Weighted down smoothly rather than cut, because a binary test pops as
//   a surface slides past the plane.
//
//   CHEBYSHEV. A probe with clear line of sight to the surface is trustworthy;
//   one with a wall in between is not, and trilinear weights alone cannot tell
//   them apart. The visibility tile's distance moments give a variance-based
//   upper bound on "is this probe visible from here", which is what stops light
//   bleeding through a thin wall into an unlit room.
//
// Geometry constants (tile resolutions, border, grid counts) arrive as uniforms
// rather than being mirrored here: the C side owns one definition and there is
// no pair to drift apart.

#include "octahedral.glsl"

uniform sampler2D giAtlasTex;
uniform int giEnabled;
uniform vec3 giGridMin;         // corner of the grid AABB, not the first probe
uniform vec3 giSpacing;         // cell size; probes sit at cell CENTRES
uniform vec3 giCounts;          // probes per axis
uniform vec2 giAtlasSize;       // atlas dimensions in texels
uniform float giIrradianceRows; // atlas rows the irradiance block occupies
uniform float giTileBorder;     // gutter width, per side
uniform vec2 giTileRes;         // interior edge: (irradiance, visibility)
uniform float giFarClip;        // the unit the visibility moments are stored in

// Atlas UV for `dir` in one probe's tile. The two blocks have different tile
// pitches, so they cannot share rows -- visibility is offset past irradiance.
vec2 giTileUV(ivec3 p, vec3 dir, bool visibility) {
    float res = visibility ? giTileRes.y : giTileRes.x;
    float pitch = res + 2.0 * giTileBorder;
    // Atlas X is the grid's X; (y,z) folds into atlas Y. Matches gi_tile_origin.
    vec2 origin = vec2(float(p.x), float(p.y) + giCounts.y * float(p.z)) * pitch;
    if (visibility)
        origin.y += giIrradianceRows;
    // octEncode's [-1,1] maps onto [0,res] in continuous texel coordinates, so
    // the direction the projection pass wrote at texel j lands exactly on j+0.5.
    vec2 oct = octEncode(dir) * 0.5 + 0.5;
    return (origin + giTileBorder + oct * res) / giAtlasSize;
}

vec3 giSampleIrradiance(vec3 worldPos, vec3 N, vec3 V) {
    float minSpacing = min(giSpacing.x, min(giSpacing.y, giSpacing.z));
    // Push the query off the surface before locating it in the grid: sampled
    // exactly at the surface, the surface is its own occluder and every probe
    // fails the Chebyshev test. Mostly along the view ray rather than the
    // normal, so the bias does not show up as a normal-shaped halo.
    vec3 biased = worldPos + (N * 0.2 + V * 0.8) * (0.75 * minSpacing);

    // Cell-centre grid: probe (i) is at grid_min + (i+0.5)*spacing, so the
    // continuous probe coordinate is the cell coordinate minus a half.
    vec3 p = (biased - giGridMin) / giSpacing - 0.5;
    vec3 base = floor(p);
    vec3 frac = clamp(p - base, 0.0, 1.0);
    ivec3 maxIdx = ivec3(giCounts) - 1;

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 pi = clamp(ivec3(base) + off, ivec3(0), maxIdx);
        vec3 probePos = giGridMin + (vec3(pi) + 0.5) * giSpacing;

        vec3 tw = mix(1.0 - frac, frac, vec3(off));
        float weight = tw.x * tw.y * tw.z;

        // Backface. The +0.2 floor keeps a probe that is only just behind the
        // surface contributing, so a shallow grazing wall does not go black.
        vec3 toProbe = normalize(probePos - worldPos);
        float facing = (dot(toProbe, N) + 1.0) * 0.5;
        weight *= facing * facing + 0.2;

        // Chebyshev. Only when the surface is FARTHER than the probe's mean
        // distance in that direction: nearer than the mean, the probe plainly
        // sees it and the bound would only add noise.
        vec3 toSurface = biased - probePos;
        float distN = length(toSurface) / giFarClip;
        vec2 moments = texture(giAtlasTex, giTileUV(pi, normalize(toSurface), true)).rg;
        if (distN > moments.x) {
            float variance = abs(moments.x * moments.x - moments.y);
            float d = distN - moments.x;
            float cheb = variance / (variance + d * d);
            weight *= cheb * cheb * cheb; // cubed: sharpen an otherwise soft falloff
        }

        // Crush the tail. A near-zero weight adds nothing but denominator, and
        // near-zero is exactly where a wrongly-included probe leaks.
        const float crush = 0.2;
        if (weight < crush)
            weight *= weight * weight / (crush * crush);

        sum += texture(giAtlasTex, giTileUV(pi, N, false)).rgb * weight;
        weightSum += weight;
    }

    return weightSum > 0.0 ? sum / weightSum : vec3(0.0);
}
