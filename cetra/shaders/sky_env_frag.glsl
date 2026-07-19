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

const float PI = 3.14159265359;
const float Rg = 6360.0;
const float Rt = 6460.0;
const float VIEW_ALTITUDE = 0.5;
const float GROUND_ALBEDO = 0.3;
const float SUN_ILLUMINANCE = 3.0; // keep in sync with sky_view

float distanceToSphere(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    if (disc < 0.0)
        return -1.0;
    return max(0.0, -r * mu + sqrt(disc));
}

bool hitsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + Rg * Rg >= 0.0;
}

vec3 transmittanceTo(float r, float mu)
{
    if (hitsGround(r, mu))
        return vec3(0.0);
    float H = sqrt(Rt * Rt - Rg * Rg);
    float rho = sqrt(max(r * r - Rg * Rg, 0.0));
    float d = distanceToSphere(r, mu, Rt);
    float d_min = Rt - r;
    float d_max = rho + H;
    return texture(transmittanceLut, vec2((d - d_min) / (d_max - d_min), rho / H)).rgb;
}

// EXACT copy of the encode in sky_background_frag (GLSL has no includes;
// keep the two in sync)
vec2 skyViewUv(vec3 dir, vec3 sun, float r)
{
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 sunHoriz = sun - up * dot(sun, up);
    if (length(sunHoriz) < 1e-4)
        sunHoriz = vec3(1.0, 0.0, 0.0);
    sunHoriz = normalize(sunHoriz);
    vec3 frameZ = cross(up, sunHoriz);

    float mu = clamp(dir.y, -1.0, 1.0);
    vec3 dHoriz = dir - up * mu;
    float azimuth = atan(dot(dHoriz, frameZ), dot(dHoriz, sunHoriz));
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
    vec3 sunT = transmittanceTo(r, sunDir.y);
    vec3 direct = sunT * max(sunDir.y, 0.0) * (GROUND_ALBEDO / PI) * SUN_ILLUMINANCE;
    FragColor = vec4(direct + groundSky * GROUND_ALBEDO, 1.0);
}
