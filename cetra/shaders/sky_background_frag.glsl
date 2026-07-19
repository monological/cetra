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

const float PI = 3.14159265359;
const float Rg = 6360.0;
const float Rt = 6460.0;
const float VIEW_ALTITUDE = 0.5;

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

// Encode a world view direction into sky-view LUT (u,v) in the sun-relative
// frame (EXACT inverse of the LUT's decodeViewDir): u is azimuth about up
// measured from the sun, v is the sqrt horizon-warped zenith.
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
        vec3 sunT = transmittanceTo(r, sunDir.y);
        sky += sunT * sunIntensity * limb;
    }

    FragColor = vec4(min(sky, vec3(30000.0)), 1.0);
}
