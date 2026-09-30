// The rain's cover (spec 13.9): whether rain reaches a world point, from the occlusion map
// shadow.c renders into its own layer of the punctual shadow array.
//
// Requires `punctualShadowMaps` declared first (punctual_shadow.glsl declares it): the map
// is a TENANT of that array, so this file names its sampler rather than declaring a second
// one, which pbr_frag has no unit for.

#include "rain_constants.glsl"

uniform int rainOcclusionLayer;   // -1 = no rain this frame; every point is open sky
uniform mat4 rainOcclusionMatrix; // world -> the map's corner of its layer

// 1 = rain reaches P, 0 = something above keeps it off. A point off the map reads the
// array's white border or the layer's cleared remainder -- open sky, which is the
// answer outside the square cover is known over.
float rainExposure(vec3 P) {
    if (rainOcclusionLayer < 0)
        return 1.0;
    vec3 pc = (rainOcclusionMatrix * vec4(P, 1.0)).xyz * 0.5 + 0.5;
    float map = textureLod(punctualShadowMaps, vec3(pc.xy, float(rainOcclusionLayer)), 0.0).r;
    return pc.z <= map + RAIN_EXPOSED_BIAS / (2.0 * RAIN_OCCLUSION_REACH) ? 1.0 : 0.0;
}
