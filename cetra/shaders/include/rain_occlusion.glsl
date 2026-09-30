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
uniform float rainCoverSpread;    // lookup-uv distance between rainExposureSoft's taps

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

// The same answer over a 3x3 of taps `rainCoverSpread` apart, for a SURFACE: the dry patch
// under an eave has a soft edge, where wind and splash carry the rain in a little.
float rainExposureSoft(vec3 P) {
    if (rainOcclusionLayer < 0)
        return 1.0;
    vec3 pc = (rainOcclusionMatrix * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    float open = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            open += rainOpenAt(pc.xy + vec2(x, y) * rainCoverSpread, pc.z);
    return open / 9.0;
}
