#version 330 core
in vec3 WorldPos;
out vec4 FragColor;

// The cloud variant of the env-cubemap face render (spec 11.0): the sky
// tap over-composited by a low-quality include march, so IBL, probes, and
// the fog ambient all see the cloud deck. Bound only on a with-clouds bake
// (slider release / startup) -- the live sun-drag path keeps the plain
// program and today's re-bake cost. The virtual ground below the horizon
// is unchanged; cloud radiance is absolute like everything in this cube.

uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform vec3 sunDir; // world-space unit vector TOWARD the sun
uniform sampler3D shapeTex;
uniform sampler3D detailTex;
uniform float coverage;
uniform float cloudType;
uniform float densityScale;

#include "noise.glsl"
#include "clouds.glsl"

void main()
{
    vec3 dir = normalize(WorldPos);
    float r = Rg + VIEW_ALTITUDE;

    if (dir.y >= 0.0) {
        vec3 sky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;
        // Env tier: 24 steps, 4 light taps, no detail erosion -- the cube
        // is 256^2 and feeds convolutions, so shape matters and texture
        // detail does not. Fixed mid-face dither (no screen pixels here).
        vec4 cloud = cloud_march(vec3(0.0, VIEW_ALTITUDE, 0.0), dir, sunDir, shapeTex, detailTex,
                                 transmittanceLut, skyViewLut, 24, 4, false, coverage, cloudType,
                                 densityScale, vec3(0.0), 0.5);
        FragColor = vec4(sky * cloud.a + cloud.rgb, 1.0);
        return;
    }

    // Below the horizon: a Lambertian virtual ground lit by the sun
    // (transmittance at the eye) plus the sky-view ground region as ambient
    // groundSky already carries SUN_ILLUMINANCE (baked into the sky-view
    // LUT); the direct bounce must match it
    vec3 groundSky = texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb;
    vec3 sunT = transmittanceToSky(transmittanceLut, r, sunDir.y);
    vec3 direct = sunT * max(sunDir.y, 0.0) * (GROUND_ALBEDO / PI) * SUN_ILLUMINANCE;
    FragColor = vec4(direct + groundSky * GROUND_ALBEDO, 1.0);
}
