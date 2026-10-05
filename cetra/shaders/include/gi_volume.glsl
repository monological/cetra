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
//
// Since spec 13.24 the world holds any number of volumes, one per place, and the
// nearest few are RESIDENT: each holds a slot of the lighting atlas and a row of
// the table below. A fragment reads ONE of them, the one it stands in; between
// places it reads the nearest, at that volume's clamped edge, which is what a
// lone volume always did outside its grid.

#include "octahedral.glsl"

// Must match GI_RESIDENT_MAX (gi_volume.h).
#define GI_SLOTS 8

uniform sampler2D giAtlasTex;
uniform int giEnabled;   // some resident volume has swept and may be sampled
uniform int giSlotCount; // resident volumes published, swept or not
// Four rows per resident volume:
//   [4s + 0] xyz the corner of its grid AABB, not the first probe; w 1 once it has swept
//   [4s + 1] xyz the cell size (probes sit at cell CENTRES); w the unit its visibility
//            moments are stored in
//   [4s + 2] xyz probes per axis; w the atlas rows its irradiance block occupies
//   [4s + 3] xy its slot's corner in the atlas, in texels
uniform vec4 giSlot[GI_SLOTS * 4];
uniform vec2 giAtlasSize;   // atlas dimensions in texels
uniform float giTileBorder; // gutter width, per side
uniform vec2 giTileRes;     // interior edge: (irradiance, visibility)

// The resident volume that answers for p: the one containing it, or the nearest
// swept one when none does. -1 when the one containing it has not swept yet,
// whose answer is the environment's -- its neighbour's edge probes saw a
// different place. Of two that contain p the first slot wins.
int giSlotAt(vec3 p) {
    int nearest = -1;
    float best = 0.0;
    for (int s = 0; s < GI_SLOTS; ++s) {
        if (s >= giSlotCount)
            break;
        vec3 lo = giSlot[4 * s].xyz;
        vec3 hi = lo + giSlot[4 * s + 1].xyz * giSlot[4 * s + 2].xyz;
        bool swept = giSlot[4 * s].w > 0.5;
        vec3 out3 = max(max(lo - p, p - hi), vec3(0.0));
        float d = dot(out3, out3);
        if (d == 0.0)
            return swept ? s : -1;
        if (swept && (nearest < 0 || d < best)) {
            nearest = s;
            best = d;
        }
    }
    return nearest;
}

// Atlas UV for `dir` in one probe's tile of slot s. The two blocks have different
// tile pitches, so they cannot share rows -- visibility is offset past irradiance.
vec2 giTileUV(int s, ivec3 p, vec3 dir, bool visibility) {
    float res = visibility ? giTileRes.y : giTileRes.x;
    float pitch = res + 2.0 * giTileBorder;
    // Atlas X is the grid's X; (y,z) folds into atlas Y. Matches gi_tile_origin.
    vec2 origin = vec2(float(p.x), float(p.y) + giSlot[4 * s + 2].y * float(p.z)) * pitch;
    if (visibility)
        origin.y += giSlot[4 * s + 2].w;
    // octEncode's [-1,1] maps onto [0,res] in continuous texel coordinates, so
    // the direction the projection pass wrote at texel j lands exactly on j+0.5.
    vec2 oct = octEncode(dir) * 0.5 + 0.5;
    return (giSlot[4 * s + 3].xy + origin + giTileBorder + oct * res) / giAtlasSize;
}

// The bounced light reaching worldPos, in `irradiance`. False when no resident
// volume answers for it, and the caller takes the environment's.
bool giIrradiance(vec3 worldPos, vec3 N, vec3 V, out vec3 irradiance) {
    irradiance = vec3(0.0);
    int s = giSlotAt(worldPos);
    if (s < 0)
        return false;
    vec3 gridMin = giSlot[4 * s].xyz;
    vec3 spacing = giSlot[4 * s + 1].xyz;
    float farClip = giSlot[4 * s + 1].w;
    vec3 counts = giSlot[4 * s + 2].xyz;

    float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    // Push the query off the surface before locating it in the grid: sampled
    // exactly at the surface, the surface is its own occluder and every probe
    // fails the Chebyshev test. Mostly along the view ray rather than the
    // normal, so the bias does not show up as a normal-shaped halo.
    vec3 biased = worldPos + (N * 0.2 + V * 0.8) * (0.75 * minSpacing);

    // Cell-centre grid: probe (i) is at grid_min + (i+0.5)*spacing, so the
    // continuous probe coordinate is the cell coordinate minus a half.
    vec3 p = (biased - gridMin) / spacing - 0.5;
    vec3 base = floor(p);
    vec3 frac = clamp(p - base, 0.0, 1.0);
    ivec3 maxIdx = ivec3(counts) - 1;

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;

    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 pi = clamp(ivec3(base) + off, ivec3(0), maxIdx);
        vec3 probePos = gridMin + (vec3(pi) + 0.5) * spacing;

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
        float distN = length(toSurface) / farClip;
        vec2 moments = texture(giAtlasTex, giTileUV(s, pi, normalize(toSurface), true)).rg;
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

        // A probe that found itself inside geometry wrote 0 into its irradiance
        // tile's alpha (spec 13.24); every other tile holds 1. What it saw is the
        // inside of a wall, and both rooms through it.
        vec4 irr = texture(giAtlasTex, giTileUV(s, pi, N, false));
        weight *= irr.a;
        sum += irr.rgb * weight;
        weightSum += weight;
    }

    irradiance = weightSum > 0.0 ? sum / weightSum : vec3(0.0);
    return true;
}

