// Shared sky radiance: the sky-view LUT sample plus the analytic
// limb-darkened sun disc, in ABSOLUTE radiance. No exposure conversion here
// -- each caller applies its own (the background pass multiplies by
// preExposure; the env bake writes absolute). Samplers are parameters, the
// atmosphere.glsl convention, so every caller binds its own units.
#include "sky_lut.glsl"

vec3 skyRadiance(vec3 dir, vec3 sunDir, float r, sampler2D skyViewLut,
                 sampler2D transmittanceLut, float sunCosRadius, float sunIntensity)
{
    vec3 sky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;

    // Sun disc: within the angular radius, add the disc radiance attenuated
    // by transmittance toward the sun at the eye, with simple limb
    // darkening. Only above the horizon.
    float cosVS = dot(dir, sunDir);
    if (cosVS > sunCosRadius && dir.y > 0.0) {
        float edge = (cosVS - sunCosRadius) / (1.0 - sunCosRadius);
        float limb = 0.4 + 0.6 * sqrt(max(edge, 0.0)); // darker toward the rim
        vec3 sunT = transmittanceToSky(transmittanceLut, r, sunDir.y);
        sky += sunT * sunIntensity * limb;
    }

    return sky;
}
