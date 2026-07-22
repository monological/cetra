#version 330 core
in vec3 WorldPos;
out vec4 FragColor;

// Environment-cubemap face render: the sky-view LUT sampled per direction,
// with a dim sun-lit virtual ground below the horizon. NO sun disc -- a
// 0.53 deg disc is a few texels at this face size, aliases the prefilter
// importance sampler into fireflies, and its direct energy already ships
// as the analytic key light (baking it here would double-count). WorldPos
// is the cube-face direction (ibl_cubemap_vert). Feeds irradiance/prefilter
// so it must stay smooth and firefly-free.

uniform sampler2D skyViewLut;
uniform sampler2D transmittanceLut;
uniform vec3 sunDir; // world-space unit vector TOWARD the sun

#include "sky_lut.glsl"

void main()
{
    vec3 dir = normalize(WorldPos);
    float r = Rg + VIEW_ALTITUDE;

    if (dir.y >= 0.0) {
        FragColor = vec4(texture(skyViewLut, skyViewUv(dir, sunDir, r)).rgb, 1.0);
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
