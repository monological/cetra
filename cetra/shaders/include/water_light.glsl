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
 * The level everywhere, not only inside `waterExtent`: that is the shoaling bed's domain, and
 * the sea runs on past it to the horizon -- forest's shelf and tree's open water both lie
 * outside it. A dry pit sunk below the level is lit as though flooded; no scene in this tree has
 * one, and one that does needs the sea told where it ends, which Water does not know.
 */
float waterDepthBelow(float y) {
    return waterDownwell == 1 ? max(waterLevel - y, 0.0) : 0.0;
}

/*
 * What of a directional light's irradiance reaches `depth` below the surface, per channel:
 * the share the surface transmits, then Beer-Lambert along the REFRACTED path, which is
 * longer than the depth by 1 / cos of the refracted angle. `toLight` is the unit direction
 * toward the light.
 *
 * Through a flat surface. The waves move where this light lands, not how much of it arrives
 * -- that is the caustics, which average 1 -- so the mean is the flat answer.
 */
vec3 waterDownwellKey(vec3 toLight, float depth) {
    if (depth <= 0.0)
        return vec3(1.0);
    float cosi = max(toLight.y, 0.0);
    float transmitted = 1.0 - fresnelDielectric(cosi, waterIor);
    float sint2 = (1.0 - cosi * cosi) / (waterIor * waterIor);
    float cost = sqrt(max(1.0 - sint2, 1.0e-4));
    return transmitted * exp(-waterAbsorption * depth / cost);
}

// What of the sky's diffuse irradiance reaches `depth` below the surface, per channel.
vec3 waterDownwellSky(float depth) {
    return exp(-waterAbsorption * WATER_SKY_DOWNWELL_PER_EXTINCTION * depth);
}
