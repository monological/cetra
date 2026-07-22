#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

// On-screen sky background: sample the sky-view LUT for the pixel's world
// view direction (encoded into the sun-relative frame), and add the
// analytic limb-darkened sun disc. Below the horizon, sample the LUT's
// ground region (a dim sun-lit virtual floor) so there is no photographic
// dome projection. Drawn with skybox_vert, so TexCoords is the world view
// direction. Output is linear HDR; the post pass tone-maps.

uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform vec3 sunDir;       // world-space unit vector TOWARD the sun
uniform float sunCosRadius; // cos of the sun's angular RADIUS
uniform float sunIntensity; // scalar disc radiance scale

#include "sky_lut.glsl"

void main()
{
    vec3 dir = normalize(TexCoords);
    float r = Rg + VIEW_ALTITUDE;

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

    FragColor = vec4(min(sky, vec3(30000.0)), 1.0);
}
