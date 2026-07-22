#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Sky-view LUT (Hillaire EGSR 2020, section 5.3): sky radiance for a fixed
// view altitude as a function of view direction, stored in a SUN-RELATIVE
// frame (azimuth measured from the sun) with a sqrt horizon-warped latitude
// so texels concentrate where the gradient is steepest. Re-baked whenever
// the sun moves. Single scattering marched directly; multiple scattering
// folded in from the Psi LUT.
//
// Units KILOMETERS.

uniform sampler2D transmittanceLut;
uniform sampler2D multiscatterLut;
uniform float sunCosZenith; // dot(sunDir, up) -- the sun's elevation

#include "sky_lut.glsl"

// Mie asymmetry: only this shader evaluates a phase function (the LUT bakes
// the phase in), so it stays local rather than joining the shared model.
const float MIE_G = 0.8;

const int SKY_STEPS = 32;

vec3 transmittanceTo(float r, float mu)
{
    return transmittanceToSky(transmittanceLut, r, mu);
}

vec3 multiscatterAt(float r, float mu_s)
{
    vec2 uv = vec2(mu_s * 0.5 + 0.5, (r - Rg) / (Rt - Rg));
    return texture(multiscatterLut, uv).rgb;
}

float rayleighPhase(float c)
{
    return 3.0 / (16.0 * PI) * (1.0 + c * c);
}

float miePhase(float c)
{
    float g = MIE_G;
    float g2 = g * g;
    return 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + c * c))
           / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Decode (u,v) -> view direction in the sun-relative frame: x toward the
// sun's azimuth, y up, z the horizontal perpendicular. u is azimuth about
// up; v is the sqrt horizon-warped zenith (v<0.5 above horizon, v>0.5
// below), matching the encode in the env/background shaders.
vec3 decodeViewDir(vec2 uv, float r)
{
    float azimuth = (uv.x * 2.0 - 1.0) * PI;

    float horizonCos = sqrt(max(r * r - Rg * Rg, 0.0)) / r; // cos(zenith->horizon)
    float horizonZenith = PI - acos(horizonCos); // > PI/2 (below straight up)

    float viewZenith;
    if (uv.y < 0.5) {
        float c = 1.0 - 2.0 * uv.y; // [0,1], 0 at horizon
        viewZenith = horizonZenith * (1.0 - c * c);
    } else {
        float c = 2.0 * uv.y - 1.0; // [0,1], 0 at horizon
        viewZenith = horizonZenith + (PI - horizonZenith) * (c * c);
    }

    float cz = cos(viewZenith);
    float sz = sin(viewZenith);
    return vec3(sz * cos(azimuth), cz, sz * sin(azimuth));
}

void main()
{
    float r = Rg + VIEW_ALTITUDE;
    vec3 viewDir = decodeViewDir(TexCoords, r);
    // Sun in the sun-relative frame: azimuth 0, elevation from sunCosZenith
    vec3 sunDir = vec3(sqrt(max(1.0 - sunCosZenith * sunCosZenith, 0.0)), sunCosZenith, 0.0);

    float mu = viewDir.y; // cos(view zenith), up = +y at the reference point
    float cosVS = dot(viewDir, sunDir);

    bool ground = hitsGround(r, mu);
    float tMax = ground ? distanceToGroundOrMiss(r, mu, Rg) : distanceToTopOrMiss(r, mu, Rt);
    if (tMax < 0.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float dt = tMax / float(SKY_STEPS);

    float phaseR = rayleighPhase(cosVS);
    float phaseM = miePhase(cosVS);

    vec3 L = vec3(0.0);
    vec3 through = vec3(1.0);

    for (int i = 0; i < SKY_STEPS; i++) {
        float t = (float(i) + 0.5) * dt;
        // Radius and local up at the sample (planet-centered)
        float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
        vec3 samplePos = vec3(0.0, r, 0.0) + viewDir * t;
        vec3 up = normalize(samplePos);
        float mu_s = dot(up, sunDir);

        vec3 rayleigh, extinction;
        float mie;
        atmosphereSample(rt - Rg, rayleigh, mie, extinction);

        vec3 stepTrans = exp(-extinction * dt);
        vec3 sunT = transmittanceTo(rt, mu_s);

        // Single scattering: sun light * phase-weighted scattering
        vec3 single = sunT * (rayleigh * phaseR + vec3(mie * phaseM));
        // Multiple scattering: isotropic Psi * total scattering
        vec3 multi = multiscatterAt(rt, mu_s) * (rayleigh + vec3(mie));

        vec3 scatterIntegral =
            (single + multi) * (vec3(1.0) - stepTrans) / max(extinction, vec3(1e-6));
        L += through * scatterIntegral;
        through *= stepTrans;
    }

    // Scale to the engine's linear range; keep HDR (bloom uses it) but
    // bound against fp16 overflow
    FragColor = vec4(min(L * SUN_ILLUMINANCE, vec3(100.0)), 1.0);
}
