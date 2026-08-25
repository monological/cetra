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

// The sky's value uniforms live in sky_radiance.glsl; the samplers and the
// deck's own pair are this program's to bind.
uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform sampler2D cloudTex;
uniform vec2 screenSize; // composite target size, for gl_FragCoord -> uv

#include "sky_radiance.glsl"

void main()
{
    vec3 dir = normalize(TexCoords);
    float r = Rg + VIEW_ALTITUDE;

    // Stars and the moon both live inside skyRadiance, so the deck
    // transmittance multiply below occludes them with no extra work.
    vec3 sky = skyRadiance(dir, r, skyViewLut, transmittanceLut);

    vec4 cloud = texture(cloudTex, gl_FragCoord.xy / screenSize);
    // Cloud radiance is absolute like the sky's; cap the in-scatter at the
    // working-space media ceiling (expressed back in absolute units, since
    // preExposure applies to the sum below).
    vec3 col = sky * cloud.a + min(cloud.rgb, vec3(WS_MEDIA_MAX) / max(preExposure, 1e-6));

    FragColor = vec4(min(col, vec3(30000.0)) * preExposure, 1.0);
}
