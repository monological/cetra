// Shared sky radiance: the sky-view LUT sample plus the analytic
// limb-darkened sun disc and the night star field, in ABSOLUTE radiance. No
// exposure conversion here -- each caller applies its own. Samplers are
// parameters, the atmosphere.glsl convention, so every caller binds its own
// units.
#include "sky_lut.glsl"
#include "stars.glsl"
#include "moon.glsl"

// The sky's own VALUE uniforms, declared here rather than passed in. Both
// callers uploaded an identical block and threaded it through an identical
// argument list, so the parameterisation carried no information and cost a row
// of same-typed positional slots -- transposing two of them at one call site
// compiles clean and renders a plausible moon with a black limb and a flat
// face, which is the failure class this file exists to make unrepresentable.
//
// The SAMPLERS stay parameters, which is the header's stated reason and
// applies to them alone: every caller binds its own units. This is the shape
// view.glsl (included by both callers, one line above) and csm.glsl already
// use.
//
// It also tightens the firefly rule from a convention into a topology fact:
// the uniforms exist only inside this file, so including it gets the whole
// sky and not including it gets none of it. There is nothing left to drift.
uniform vec3 sunDir;          // world-space unit vector TOWARD the sun
uniform float sunCosRadius;   // cos of the angular RADIUS shared by both discs
uniform float sunIntensity;   // scalar disc radiance scale
uniform mat3 starFrame;       // world dir -> celestial frame (latitude + hour angle)
uniform float starIntensity;  // star radiance scale; 0 = daylight / disabled
uniform vec3 moonDir;         // world-space unit vector TOWARD the moon
uniform float moonIntensity;  // disc radiance scale; 0 = new moon / day / disabled
uniform float moonEarthshine; // 1 = the dark limb is Earth-lit, 0 = black
uniform float moonMaria;      // 1 = the face is textured, 0 = uniform

vec3 skyRadiance(vec3 dir, float r, sampler2D skyViewLut, sampler2D transmittanceLut)
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
    // CPU-side night ramp, so daylight is an exact zero. The tint is applied
    // at partial saturation -- a star look decision, so the constant lives
    // with the rest of them in stars.glsl.
    if (starIntensity > 0.0) {
        vec3 t = transmittanceToSky(transmittanceLut, r, dir.y);
        t = mix(vec3(dot(t, vec3(0.2126, 0.7152, 0.0722))), t, STAR_TINT_SATURATION);
        sky += starRadiance(starFrame * dir) * t * starIntensity;
    }

    // The moon: the sun-disc pattern a THIRD time (spec 11.82), and in the
    // background shaders only for the second time -- a half-degree disc
    // aliases the GGX prefilter importance sampler into fireflies, and its
    // energy already ships as the analytic moon light. moonIntensity carries
    // the CPU-side phase law and the shared night ramp, so a new moon and a
    // daylit sky are both an exact zero and this block costs nothing. It
    // reuses sunCosRadius: the two discs subtend 0.53 and 0.52 degrees.
    if (moonIntensity > 0.0) {
        // The derivative is taken HERE, under a uniform condition and above
        // the per-pixel disc test -- the stars' rule and its reason.
        float pixel = length(fwidth(dir));
        float cosVM = dot(dir, moonDir);
        if (cosVM > sunCosRadius && dir.y > 0.0) {
            float edge = (cosVM - sunCosRadius) / (1.0 - sunCosRadius);
            vec3 moonT = transmittanceToSky(transmittanceLut, r, moonDir.y);
            sky += moonT * moonIntensity *
                   moonDisc(edge, dir, moonDir, sunDir, sunCosRadius, pixel, moonEarthshine,
                            moonMaria);
        }
    }

    return sky;
}
