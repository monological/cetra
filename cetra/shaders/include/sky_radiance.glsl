// Shared sky radiance: the sky-view LUT sample plus the analytic
// limb-darkened sun disc and the night star field, in ABSOLUTE radiance. No
// exposure conversion here -- each caller applies its own. Samplers are
// parameters, the atmosphere.glsl convention, so every caller binds its own
// units.
#include "sky_lut.glsl"
#include "stars.glsl"

vec3 skyRadiance(vec3 dir, vec3 sunDir, float r, sampler2D skyViewLut,
                 sampler2D transmittanceLut, float sunCosRadius, float sunIntensity,
                 mat3 starFrame, float starIntensity)
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

    // Stars: the sun-disc pattern a second time (spec 11.79). The same
    // transmittance dims them toward the horizon and zeroes them where the
    // ray hits ground; sitting inside `sky` is what puts them under the
    // clouds variant's deck transmittance. starIntensity carries the
    // CPU-side night ramp, so daylight is an exact zero. The TINT is applied
    // at half saturation: the full spectral extinction is what paints the
    // sun orange at the horizon, and on a whole field of point sources it
    // reads as grime rather than as reddening.
    if (starIntensity > 0.0) {
        vec3 t = transmittanceToSky(transmittanceLut, r, dir.y);
        t = mix(vec3(dot(t, vec3(0.2126, 0.7152, 0.0722))), t, 0.35);
        sky += starRadiance(starFrame * dir) * t * starIntensity;
    }

    return sky;
}
