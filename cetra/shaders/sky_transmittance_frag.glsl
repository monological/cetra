#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Atmosphere transmittance LUT (Hillaire EGSR 2020 / Bruneton 2017).
// For a point at radius r and a ray with cos-zenith mu that does NOT hit
// the ground, integrate the optical depth to the top of the atmosphere and
// store exp(-depth). Baked ONCE: transmittance is sun-independent.
//
// All distances are in KILOMETERS: the planetary magnitudes (Rt ~ 6460)
// stay float-safe and the scattering coefficients below are per-km.
// The atmosphere constants are duplicated across the sky_* shaders (GLSL
// has no includes here) and MUST stay in sync.


// Earth
const float Rg = 6360.0; // ground radius (km)
const float Rt = 6460.0; // top-of-atmosphere radius (km)

// Rayleigh: scattering == extinction (no absorption), scale height 8 km
const vec3 RAYLEIGH_SCATTER = vec3(5.802e-3, 13.558e-3, 33.1e-3);
const float RAYLEIGH_H = 8.0;
// Mie: extinction = scattering / 0.9 (Hillaire's single-albedo fit)
const float MIE_SCATTER = 3.996e-3;
const float MIE_EXTINCTION = MIE_SCATTER / 0.9;
const float MIE_H = 1.2;
// Ozone: absorption only, tent profile centered at 25 km, half-width 15 km
const vec3 OZONE_ABSORB = vec3(0.650e-3, 1.881e-3, 0.085e-3);
const float OZONE_CENTER = 25.0;
const float OZONE_WIDTH = 15.0;

const int TRANSMITTANCE_STEPS = 40;

// Distance from a point at radius r along cos-zenith mu to the sphere of
// radius R (largest root; caller guarantees an intersection exists)
float distanceToSphere(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

// Bruneton's transmittance UV inverse mapping: distributes texel precision
// toward the horizon, where transmittance varies fastest and fp16 would
// otherwise band. x = normalized distance-to-top, y = rho / H.
void uvToTransmittanceParams(vec2 uv, out float r, out float mu)
{
    float H = sqrt(Rt * Rt - Rg * Rg);
    float rho = H * uv.y;
    r = sqrt(rho * rho + Rg * Rg);

    float d_min = Rt - r;
    float d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

// Extinction coefficient of the mixed atmosphere at altitude h (km)
vec3 extinctionAt(float h)
{
    float rayleighD = exp(-h / RAYLEIGH_H);
    float mieD = exp(-h / MIE_H);
    float ozoneD = max(0.0, 1.0 - abs(h - OZONE_CENTER) / OZONE_WIDTH);
    return RAYLEIGH_SCATTER * rayleighD + vec3(MIE_EXTINCTION * mieD) + OZONE_ABSORB * ozoneD;
}

void main()
{
    float r, mu;
    uvToTransmittanceParams(TexCoords, r, mu);

    // March from (r, mu) to the top boundary
    float dist = distanceToSphere(r, mu, Rt);
    float dt = dist / float(TRANSMITTANCE_STEPS);

    vec3 depth = vec3(0.0);
    for (int i = 0; i < TRANSMITTANCE_STEPS; i++) {
        float t = (float(i) + 0.5) * dt;
        // Radius at parameter t along the ray (law of cosines)
        float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
        depth += extinctionAt(rt - Rg) * dt;
    }

    FragColor = vec4(exp(-depth), 1.0);
}
