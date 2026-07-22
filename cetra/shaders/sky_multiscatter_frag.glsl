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
// Units are KILOMETERS.

uniform sampler2D transmittanceLut;

#include "atmosphere.glsl"

const int SPHERE_DIRS = 64; // 8x8 lat-long quadrature over the sphere
const int MARCH_STEPS = 20;

// No ground cut here: this bake integrates THROUGH, unlike the three shaders
// that sample the sky-view LUT.
vec3 transmittanceTo(float r, float mu)
{
    return transmittanceLookup(transmittanceLut, r, mu);
}

// Combined in-scatter (Rayleigh + Mie) -- the isotropic-phase approximation
// this LUT is built on does not need them separated.
void scatteringAt(float h, out vec3 scatter, out vec3 extinction)
{
    vec3 rayleigh;
    float mie;
    atmosphereSample(h, rayleigh, mie, extinction);
    scatter = rayleigh + vec3(mie);
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
        float tMax =
            ground ? distanceToGroundClamped(r, dir.y, Rg) : distanceToTopClamped(r, dir.y, Rt);
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
