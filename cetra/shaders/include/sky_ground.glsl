// The env cube's below-horizon virtual ground: a Lambertian floor lit by the
// sun (transmittance at the eye) plus the sky-view ground region as ambient.
// groundSky already carries SUN_ILLUMINANCE (baked into the sky-view LUT);
// the direct bounce must match it. Absolute radiance, samplers as
// parameters, the atmosphere.glsl convention.
//
// `overcast` is the deck (spec 13.7). Under it the LUT's ground region already
// holds the floor the overcast dome lights, so the ground blends toward that
// alone: the direct sun fades with the clear sky it came through, and no dome
// term is added a second time here.
#include "sky_lut.glsl"

vec3 skyVirtualGround(vec3 dir, vec3 sunDir, float r, sampler2D skyViewLut,
                      sampler2D transmittanceLut, float overcast)
{
    vec3 groundSky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;
    vec3 sunT = transmittanceToSky(transmittanceLut, r, sunDir.y);
    vec3 direct = sunT * max(sunDir.y, 0.0) * (GROUND_ALBEDO / PI) * SUN_ILLUMINANCE;
    vec3 clearGround = direct + groundSky * GROUND_ALBEDO;
    if (overcast > 0.0)
        return mix(clearGround, groundSky, overcast);
    return clearGround;
}
