// Sky-view LUT addressing, shared by the two shaders that SAMPLE the LUT
// (sky_env_frag bakes the environment cubemap, sky_background_frag draws the
// on-screen sky). Both encoded a world direction into the LUT's sun-relative
// frame with byte-identical code, under a comment asking the next person to
// keep the copies in step by hand.
//
// This is the exact inverse of the decode in sky_view_frag, which BUILDS the
// LUT. Those two must agree or the sky is sampled at the wrong angle; the
// decode stays there because nothing else needs it.
#include "atmosphere.glsl"

// u is azimuth about up measured from the sun; v is the sqrt horizon-warped
// zenith, which concentrates texels where the gradient is steepest.
vec2 skyViewUv(vec3 dir, vec3 sun, float r)
{
    vec3 up = vec3(0.0, 1.0, 0.0);

    // Sun-relative horizontal frame; degenerate near the zenith -> pick any
    vec3 sunHoriz = sun - up * dot(sun, up);
    if (length(sunHoriz) < 1e-4)
        sunHoriz = vec3(1.0, 0.0, 0.0);
    sunHoriz = normalize(sunHoriz);
    vec3 frameZ = cross(up, sunHoriz);

    float mu = clamp(dir.y, -1.0, 1.0);
    vec3 dHoriz = dir - up * mu;
    float azimuth = atan(dot(dHoriz, frameZ), dot(dHoriz, sunHoriz)); // [-PI, PI]
    float u = azimuth / (2.0 * PI) + 0.5;

    float viewZenith = acos(mu);
    float horizonCos = sqrt(max(r * r - Rg * Rg, 0.0)) / r;
    float horizonZenith = PI - acos(horizonCos);

    float v;
    if (viewZenith < horizonZenith) {
        float c = sqrt(1.0 - viewZenith / horizonZenith);
        v = 0.5 * (1.0 - c);
    } else {
        float c = sqrt((viewZenith - horizonZenith) / (PI - horizonZenith));
        v = 0.5 + 0.5 * c;
    }
    return vec2(u, v);
}

