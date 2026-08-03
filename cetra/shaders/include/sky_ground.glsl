// The env cube's below-horizon virtual ground: a Lambertian floor lit by the
// sun (transmittance at the eye) plus the sky-view ground region as ambient.
// groundSky already carries SUN_ILLUMINANCE (baked into the sky-view LUT);
// the direct bounce must match it. Absolute radiance, samplers as
// parameters, the atmosphere.glsl convention.
#include "sky_lut.glsl"

vec3 skyVirtualGround(vec3 dir, vec3 sunDir, float r, sampler2D skyViewLut,
                      sampler2D transmittanceLut)
{
    vec3 groundSky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;
    vec3 sunT = transmittanceToSky(transmittanceLut, r, sunDir.y);
    vec3 direct = sunT * max(sunDir.y, 0.0) * (GROUND_ALBEDO / PI) * SUN_ILLUMINANCE;
    return direct + groundSky * GROUND_ALBEDO;
}
