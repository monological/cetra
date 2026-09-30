// The env cube's below-horizon virtual ground: a Lambertian floor lit by the
// sun (transmittance at the eye) plus the sky-view ground region as ambient.
// groundSky already carries SUN_ILLUMINANCE (baked into the sky-view LUT);
// the direct bounce must match it. Absolute radiance, samplers as
// parameters, the atmosphere.glsl convention.
//
// Under an overcast deck (spec 13.7) the LUT's ground region already holds the
// floor the dome lights, albedo x deckFloor. Scaling groundSky by the albedo
// below keeps only an albedo's share of that floor, so the rest, (1 - albedo),
// is added back: the ground stays linear in the deck rather than weighted by it
// twice.
#include "sky_lut.glsl"
#include "sky_deck.glsl"

vec3 skyVirtualGround(vec3 dir, vec3 sunDir, float r, sampler2D skyViewLut,
                      sampler2D transmittanceLut)
{
    vec3 groundSky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;
    vec3 sunT = transmittanceToSky(transmittanceLut, r, sunDir.y);
    vec3 direct =
        sunT * max(sunDir.y, 0.0) * (GROUND_ALBEDO / PI) * SUN_ILLUMINANCE * deckSunScale;
    return direct + groundSky * GROUND_ALBEDO + (1.0 - GROUND_ALBEDO) * GROUND_ALBEDO * deckFloor;
}
