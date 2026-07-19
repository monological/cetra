#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Multiple-scattering LUT (Hillaire EGSR 2020, section 5.2): Psi_ms as a
// function of (sun cos-zenith, altitude), under the paper's isotropy
// approximations: the 2nd-order in-scatter L2 and the transfer factor f_ms
// are integrated over the sphere with an isotropic phase, and the infinite
// bounce series closes as Psi = L2 / (1 - f_ms). Baked ONCE: the sun's
// zenith angle is an AXIS of this LUT, not an input.
//
// Units are KILOMETERS. Atmosphere constants duplicated across the sky_*
// shaders (no GLSL includes) and MUST stay in sync.

uniform sampler2D transmittanceLut;

const float PI = 3.14159265359;

const float Rg = 6360.0;
const float Rt = 6460.0;

const vec3 RAYLEIGH_SCATTER = vec3(5.802e-3, 13.558e-3, 33.1e-3);
const float RAYLEIGH_H = 8.0;
const float MIE_SCATTER = 3.996e-3;
const float MIE_EXTINCTION = MIE_SCATTER / 0.9;
const float MIE_H = 1.2;
const vec3 OZONE_ABSORB = vec3(0.650e-3, 1.881e-3, 0.085e-3);
const float OZONE_CENTER = 25.0;
const float OZONE_WIDTH = 15.0;

const float GROUND_ALBEDO = 0.3;

const int SPHERE_DIRS = 64; // 8x8 lat-long quadrature over the sphere
const int MARCH_STEPS = 20;

// FAR intersection (exit through the atmosphere top)
float distanceToTop(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

// NEAR intersection (first ground hit ahead of the ray -- so the march
// stops at the surface instead of continuing through the planet to the far
// root, which produced huge/NaN samples)
float distanceToGround(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    return max(0.0, -r * mu - sqrt(max(disc, 0.0)));
}

// Does the ray from radius r with cos-zenith mu hit the ground?
bool hitsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + Rg * Rg >= 0.0;
}

// Bruneton transmittance UV forward mapping (inverse lives in the
// transmittance bake shader; keep in sync)
vec2 transmittanceUv(float r, float mu)
{
    float H = sqrt(Rt * Rt - Rg * Rg);
    float rho = sqrt(max(r * r - Rg * Rg, 0.0));
    float d = distanceToTop(r, mu, Rt);
    float d_min = Rt - r;
    float d_max = rho + H;
    return vec2((d - d_min) / (d_max - d_min), rho / H);
}

vec3 transmittanceTo(float r, float mu)
{
    return texture(transmittanceLut, transmittanceUv(r, mu)).rgb;
}

void scatteringAt(float h, out vec3 scatter, out vec3 extinction)
{
    float rayleighD = exp(-h / RAYLEIGH_H);
    float mieD = exp(-h / MIE_H);
    float ozoneD = max(0.0, 1.0 - abs(h - OZONE_CENTER) / OZONE_WIDTH);
    scatter = RAYLEIGH_SCATTER * rayleighD + vec3(MIE_SCATTER * mieD);
    extinction = RAYLEIGH_SCATTER * rayleighD + vec3(MIE_EXTINCTION * mieD)
                 + OZONE_ABSORB * ozoneD;
}

void main()
{
    // LUT axes: x = sun cos-zenith remapped from [-1,1], y = altitude
    float mu_s = TexCoords.x * 2.0 - 1.0;
    float r = Rg + TexCoords.y * (Rt - Rg);
    vec3 sunDir = vec3(sqrt(max(1.0 - mu_s * mu_s, 0.0)), mu_s, 0.0);

    const float ISO_PHASE = 1.0 / (4.0 * PI);

    vec3 L2 = vec3(0.0);   // 2nd-order in-scatter reaching the point
    vec3 fms = vec3(0.0);  // energy transfer factor for one more bounce

    // Uniform sphere quadrature: 8 azimuth x 8 cos-zenith strata
    for (int i = 0; i < SPHERE_DIRS; i++) {
        float azi = 2.0 * PI * (float(i / 8) + 0.5) / 8.0;
        float cz = -1.0 + 2.0 * (float(i - (i / 8) * 8) + 0.5) / 8.0;
        float sz = sqrt(max(1.0 - cz * cz, 0.0));
        vec3 dir = vec3(sz * cos(azi), cz, sz * sin(azi));
        float dOmega = 4.0 * PI / float(SPHERE_DIRS);

        bool ground = hitsGround(r, dir.y);
        float tMax = ground ? distanceToGround(r, dir.y, Rg) : distanceToTop(r, dir.y, Rt);
        float dt = tMax / float(MARCH_STEPS);

        vec3 through = vec3(1.0); // transmittance from the point to the sample
        vec3 Ldir = vec3(0.0);    // single-scattered light arriving along dir
        vec3 fdir = vec3(0.0);    // scatter mass along dir (transfer)

        for (int s = 0; s < MARCH_STEPS; s++) {
            float t = (float(s) + 0.5) * dt;
            float rt = sqrt(r * r + t * t + 2.0 * r * t * dir.y);
            // Sun cos-zenith at the sample: the local up is the normalized
            // planet-centered position
            vec3 up = normalize(vec3(0.0, r, 0.0) + dir * t);
            float mu_s_t = clamp(dot(up, sunDir), -1.0, 1.0);

            vec3 scatter, extinction;
            scatteringAt(rt - Rg, scatter, extinction);
            vec3 stepTrans = exp(-extinction * dt);
            vec3 integ = (vec3(1.0) - stepTrans) / max(extinction, vec3(1e-6));

            // Sun light reaching the sample (zero if the sun is below the
            // local horizon; the transmittance LUT rows only cover
            // non-ground rays, hence the explicit test)
            vec3 sunT = hitsGround(rt, mu_s_t) ? vec3(0.0)
                                               : transmittanceTo(rt, mu_s_t);

            Ldir += through * scatter * integ * sunT * ISO_PHASE;
            fdir += through * scatter * integ;
            through *= stepTrans;
        }

        if (ground) {
            // Sun light bounced off the ground back toward the point
            vec3 groundPoint = vec3(0.0, r, 0.0) + dir * tMax;
            float mu_g = clamp(dot(normalize(groundPoint), sunDir), -1.0, 1.0);
            vec3 sunT = mu_g > 0.0 ? transmittanceTo(Rg, mu_g) : vec3(0.0);
            Ldir += through * sunT * max(mu_g, 0.0) * (GROUND_ALBEDO / PI);
        }

        L2 += Ldir * ISO_PHASE * dOmega;
        fms += fdir * ISO_PHASE * dOmega;
    }

    // Close the infinite bounce series. Psi is a normalized transfer factor
    // (per unit sun illuminance): small stored values are expected -- the
    // sky-view march scales them back against the real sun.
    vec3 psi = L2 / max(vec3(1.0) - fms, vec3(1e-4));
    FragColor = vec4(psi, 1.0);
}
