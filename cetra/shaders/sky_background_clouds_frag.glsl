#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

#include "view.glsl"

// The cloud variant of the sky background (spec 11.0): the shared sky
// radiance body over-composited by the half-res cloud march,
// sky * cloud.a + cloud.rgb. Bound only when clouds are on -- the plain
// sky_background program stays byte-untouched for the off path. A plain
// bilinear tap upsamples the cloud texture: cloud content is smooth and
// sky-only, and geometry silhouettes are resolved by this draw's own
// depth test, not by the texture.

uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform sampler2D cloudTex;
uniform vec2 screenSize;    // composite target size, for gl_FragCoord -> uv
uniform vec3 sunDir;        // world-space unit vector TOWARD the sun
uniform float sunCosRadius; // cos of the sun's angular RADIUS
uniform float sunIntensity; // scalar disc radiance scale

#include "sky_radiance.glsl"

void main()
{
    vec3 dir = normalize(TexCoords);
    float r = Rg + VIEW_ALTITUDE;

    vec3 sky =
        skyRadiance(dir, sunDir, r, skyViewLut, transmittanceLut, sunCosRadius, sunIntensity);

    vec4 cloud = texture(cloudTex, gl_FragCoord.xy / screenSize);
    // Cloud radiance is absolute like the sky's; cap the in-scatter at the
    // working-space media ceiling (expressed back in absolute units, since
    // preExposure applies to the sum below).
    vec3 col = sky * cloud.a + min(cloud.rgb, vec3(WS_MEDIA_MAX) / max(preExposure, 1e-6));

    FragColor = vec4(min(col, vec3(30000.0)) * preExposure, 1.0);
}
