#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Atmosphere transmittance LUT (Hillaire EGSR 2020 / Bruneton 2017).
// For a point at radius r and a ray with cos-zenith mu that does NOT hit
// the ground, integrate the optical depth to the top of the atmosphere and
// store exp(-depth). Baked ONCE: transmittance is sun-independent.
//
#include "atmosphere.glsl"

const int TRANSMITTANCE_STEPS = 40;

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
    vec3 rayleigh;
    float mie;
    vec3 extinction;
    atmosphereSample(h, rayleigh, mie, extinction);
    return extinction;
}

void main()
{
    float r, mu;
    uvToTransmittanceParams(TexCoords, r, mu);

    // March from (r, mu) to the top boundary
    float dist = distanceToTopClamped(r, mu, Rt);
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
