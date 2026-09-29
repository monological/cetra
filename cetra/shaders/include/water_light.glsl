// What of the light falling on the sea reaches a point below it (spec 13.4).
//
// Read by the water, and by every lit surface through pbr_frag. A submerged surface is lit
// in its own program, which knows nothing else about the sea, so the weakening of the light
// on its way down has to be something that program can evaluate from a depth alone -- and
// the water's caustics have to judge the key's share against the same light, or the pattern
// is laid over a bed lit one way and weighed as though lit another.
//
// Scalars and one include only: pbr_frag is at its sixteen samplers.

#include "fresnel.glsl"

uniform float waterIor;
uniform vec3 waterAbsorption; // extinction per world unit, per channel
// 1 = a surface under the level is lit through the water above it: a sea is drawn and its
// downwell switch is on. 0 leaves every surface in air.
uniform int waterDownwell;

/*
 * How much weaker the sky's diffuse light gets per unit of extinction, against a collimated
 * beam going straight down.
 *
 * Two effects pull opposite ways. Diffuse light arrives at every angle, so its mean path
 * through a layer is longer than the layer by about 1/0.8. And what the water scatters out of
 * it is mostly scattered onward, still downward, so only part of the scattering is a loss.
 * Clearwater writes the pair as (sigma_a + 0.4 sigma_s) * 1.25; over its three channels that
 * comes to 0.92 to 1.2 times sigma_t. We carry only sigma_t, so the mean stands in.
 */
const float WATER_SKY_DOWNWELL_PER_EXTINCTION = 1.0;

/*
 * How far below the still surface `y` is, in world units; 0 above it, or with no sea.
 *
 * Measured against the level everywhere, not only inside `waterExtent`: that is the shoaling
 * bed's domain, and the sea runs on past it to the horizon. So a dry pit sunk below the level is
 * lit as though flooded; telling the two apart needs the sea to know where it ends, which Water
 * does not.
 */
float waterDepthBelow(float y) {
    return waterDownwell == 1 ? max(waterLevel - y, 0.0) : 0.0;
}

/*
 * A directional light's beam once through a flat surface: its direction in the water, the cosine
 * of that from straight down, and the share of its irradiance the surface transmits. `toLight`
 * is the unit direction toward the light.
 *
 * Through a flat surface. The waves move where this light lands, not how much of it arrives
 * -- that is the caustics, which average 1 -- so the mean is the flat answer.
 */
void waterKeyThroughSurface(vec3 toLight, out vec3 inWater, out float cosT, out float transmit) {
    float cosi = max(toLight.y, 0.0);
    transmit = 1.0 - fresnelDielectric(cosi, waterIor);
    float sint2 = (1.0 - cosi * cosi) / (waterIor * waterIor);
    cosT = sqrt(max(1.0 - sint2, 1.0e-4));
    // Snell's law: the part along the surface shrinks by the index, the rest points down.
    inWater = vec3(-toLight.x / waterIor, -cosT, -toLight.z / waterIor);
}

// What of that beam's irradiance reaches `depth` below the surface, per channel: Beer-Lambert
// along the refracted path, longer than the depth by 1 / cosT.
vec3 waterDownwellThrough(float transmit, float cosT, float depth) {
    return depth > 0.0 ? transmit * exp(-waterAbsorption * depth / cosT) : vec3(1.0);
}

// The two above for one light at one point.
vec3 waterDownwellKey(vec3 toLight, float depth) {
    if (depth <= 0.0)
        return vec3(1.0);
    vec3 inWater;
    float cosT, transmit;
    waterKeyThroughSurface(toLight, inWater, cosT, transmit);
    return waterDownwellThrough(transmit, cosT, depth);
}

// What of the sky's diffuse irradiance reaches `depth` below the surface, per channel.
vec3 waterDownwellSky(float depth) {
    return depth > 0.0 ? exp(-waterAbsorption * WATER_SKY_DOWNWELL_PER_EXTINCTION * depth)
                       : vec3(1.0);
}
