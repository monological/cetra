// The rain's cover (spec 13.9): whether rain reaches a world point, from the occlusion map
// shadow.c renders into its own layer of the punctual shadow array.
//
// Requires `punctualShadowMaps` declared first (punctual_shadow.glsl declares it): the map
// is a TENANT of that array, so this file names its sampler rather than declaring a second
// one, which pbr_frag has no unit for.

#include "rain_constants.glsl"

// What a fully porous, fully wet surface loses of its diffuse albedo: the top of the 25-50%
// band Lagarde gives for natural materials. The shore's sand is 0.38 of the same scale.
const float RAIN_POROSITY_DARKEN = 0.5;
// Where a surface starts to hold a film and where it holds a full one, as the up component of
// its normal: a roof pitched past about 70 degrees sheds, a road holds.
const float RAIN_FILM_UP_MIN = 0.35;
const float RAIN_FILM_UP_FULL = 0.85;

uniform int rainOcclusionLayer;   // -1 = no rain this frame; every point is open sky
uniform mat4 rainOcclusionMatrix; // world -> the map's corner of its layer
uniform float rainCoverSpread;    // lookup-uv distance between the blocker search's taps
uniform float rainUvPerMetre;     // lookup uv across one metre of the map's footprint

// The rain does not fall in parallel lines: turbulence and gusts spread its direction by a
// few degrees, so the dry patch an occluder leaves blurs in proportion to how far above the
// surface it is. A car on the road keeps a sharp dry outline; a lamp head five metres up
// leaves nothing you could find. The tangent of about eight degrees.
const float RAIN_SPREAD_TAN = 0.14;
const int RAIN_COVER_TAPS = 12;

// One tap: whether the map puts anything between the sky and a point at `uv`, depth `z`.
float rainOpenAt(vec2 uv, float z) {
    float map = textureLod(punctualShadowMaps, vec3(uv, float(rainOcclusionLayer)), 0.0).r;
    return z <= map + RAIN_EXPOSED_BIAS / (2.0 * RAIN_OCCLUSION_REACH) ? 1.0 : 0.0;
}

// 1 = rain reaches P, 0 = something above keeps it off. A point off the map reads the
// array's white border or the layer's cleared remainder -- open sky, which is the
// answer outside the square cover is known over.
float rainExposure(vec3 P) {
    if (rainOcclusionLayer < 0)
        return 1.0;
    vec3 pc = (rainOcclusionMatrix * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    return rainOpenAt(pc.xy, pc.z);
}

// The same answer for a SURFACE, softened the way a shadow is softened by the size of its
// light (PCSS): first a search for what covers the point and how far above it that is, then a
// disk of taps as wide as the rain's spread over that height. `rotation` turns the disk per
// pixel, which is what keeps a thin occluder from printing as a row of ghost copies -- a
// regular grid of taps wider than a texel finds the same one-texel line once per row.
float rainExposureSoft(vec3 P, float rotation) {
    if (rainOcclusionLayer < 0)
        return 1.0;
    vec3 pc = (rainOcclusionMatrix * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    float bias = RAIN_EXPOSED_BIAS / (2.0 * RAIN_OCCLUSION_REACH);
    float blockers = 0.0;
    float blockerDepth = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 uv = pc.xy + vec2(x, y) * rainCoverSpread;
            float map = textureLod(punctualShadowMaps, vec3(uv, float(rainOcclusionLayer)), 0.0).r;
            if (pc.z > map + bias) {
                blockers += 1.0;
                blockerDepth += map;
            }
        }
    }
    if (blockers == 0.0)
        return 1.0;
    // Metres between the surface and what covers it, along the rain's own travel.
    float height = (pc.z - blockerDepth / blockers) * 2.0 * RAIN_OCCLUSION_REACH;
    float radius = max(rainCoverSpread, height * RAIN_SPREAD_TAN * rainUvPerMetre);
    float open = 0.0;
    for (int i = 0; i < RAIN_COVER_TAPS; i++) {
        // A Vogel disk: the golden angle between taps, radius growing as the square root, so
        // the taps cover the disk evenly with no grid for an edge to line up with.
        float r = radius * sqrt((float(i) + 0.5) / float(RAIN_COVER_TAPS));
        float a = float(i) * 2.39996323 + rotation;
        open += rainOpenAt(pc.xy + r * vec2(cos(a), sin(a)), pc.z);
    }
    return open / float(RAIN_COVER_TAPS);
}
