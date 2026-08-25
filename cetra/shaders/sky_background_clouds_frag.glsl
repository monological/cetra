#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

#include "view.glsl"

// The cloud variant of the sky background (spec 11.0): the shared sky
// radiance body over-composited by the half-res cloud march,
// sky * cloud.a + cloud.rgb. A separate program from the plain background
// so the off path never carries a cloud sampler. A plain bilinear tap
// upsamples the cloud texture: cloud content is smooth and sky-only, and
// geometry silhouettes are resolved by this draw's own depth test, not by
// the texture.

uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform sampler2D cloudTex;
uniform vec2 screenSize;    // composite target size, for gl_FragCoord -> uv
uniform vec3 sunDir;        // world-space unit vector TOWARD the sun
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

    // Stars and the moon both live inside skyRadiance, so the deck
    // transmittance multiply below occludes them with no extra work.
    vec3 sky = skyRadiance(dir, sunDir, r, skyViewLut, transmittanceLut, sunCosRadius,
                           sunIntensity, starFrame, starIntensity, moonDir, moonIntensity,
                           moonEarthshine, moonMaria);

    vec4 cloud = texture(cloudTex, gl_FragCoord.xy / screenSize);
    // Cloud radiance is absolute like the sky's; cap the in-scatter at the
    // working-space media ceiling (expressed back in absolute units, since
    // preExposure applies to the sum below).
    vec3 col = sky * cloud.a + min(cloud.rgb, vec3(WS_MEDIA_MAX) / max(preExposure, 1e-6));

    FragColor = vec4(min(col, vec3(30000.0)) * preExposure, 1.0);
}
