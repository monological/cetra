#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

#include "view.glsl"

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
uniform mat3 starFrame;     // world dir -> celestial frame (latitude + hour angle)
uniform float starIntensity; // star radiance scale; 0 = daylight / disabled
uniform vec3 moonDir;         // world-space unit vector TOWARD the moon
uniform float moonIntensity;  // disc radiance scale; 0 = new moon / day / disabled
uniform float moonEarthshine; // 1 = the dark limb is Earth-lit, 0 = black
uniform float moonMaria;      // 1 = the face is textured, 0 = uniform

#include "sky_radiance.glsl"

void main()
{
    vec3 dir = normalize(TexCoords);
    float r = Rg + VIEW_ALTITUDE;

    vec3 sky = skyRadiance(dir, sunDir, r, skyViewLut, transmittanceLut, sunCosRadius,
                           sunIntensity, starFrame, starIntensity, moonDir, moonIntensity,
                           moonEarthshine, moonMaria);

    FragColor = vec4(min(sky, vec3(30000.0)) * preExposure, 1.0);
}
