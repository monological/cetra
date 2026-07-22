// Hillaire (EGSR 2020) / Bruneton (2017) atmosphere model: the constants and
// the geometry every sky_* shader shares. ONE copy -- the transmittance LUT
// bake and its four consumers must integrate the same atmosphere or the sky is
// energetically wrong in a way that still looks plausible.
//
// All distances are in KILOMETERS: the planetary magnitudes (Rt ~ 6460) stay
// float-safe and the scattering coefficients below are per-km.
//
// NOTE: sky.c:62-75 keeps an independent C mirror of Rg/Rt and the Rayleigh/
// Mie/Ozone coefficients for the CPU-side sun transmittance (there is no GPU
// readback). Changing a value here without changing that one silently desyncs
// the analytic key light's colour and intensity from the sky it stands under.

const float PI = 3.14159265359;

// Earth
const float Rg = 6360.0; // ground radius (km)
const float Rt = 6460.0; // top-of-atmosphere radius (km)

// Fixed observer altitude (km): cetra cameras never leave the ground, so the
// sky-view LUT's altitude input is constant.
const float VIEW_ALTITUDE = 0.5;

const float GROUND_ALBEDO = 0.3;

// Sun illuminance: the atmosphere integral is computed per unit sun
// illuminance (physical sky radiance ~ 0.04), so scale into the engine's
// linear range (daytime zenith ~ a couple units, comparable to a studio HDR).
const float SUN_ILLUMINANCE = 3.0;

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

// Ray/sphere intersection comes in two contracts, and the sky shaders had
// forked into BOTH under three different names before this chunk existed
// (distanceToSphere in two files, distanceToTop in two more, with two distinct
// bodies). They are not interchangeable, so both are spelled out here and each
// caller picks deliberately.
//
// ...Clamped: never negative; a miss reads as 0 distance.
// ...OrMiss:  returns -1.0 on a miss, so the caller can branch.
//
// Far root (exit through sphere R, for an observer inside it).
float distanceToTopClamped(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

float distanceToTopOrMiss(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    if (disc < 0.0)
        return -1.0;
    return max(0.0, -r * mu + sqrt(disc));
}

// Near root (first hit ahead of the ray). Used for the ground so a march stops
// at the surface instead of continuing through the planet to the far root,
// which produced huge/NaN samples.
float distanceToGroundClamped(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    return max(0.0, -r * mu - sqrt(max(disc, 0.0)));
}

float distanceToGroundOrMiss(float r, float mu, float R)
{
    float disc = r * r * (mu * mu - 1.0) + R * R;
    if (disc < 0.0)
        return -1.0;
    return max(0.0, -r * mu - sqrt(disc));
}

bool hitsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + Rg * Rg >= 0.0;
}

// Rayleigh scattering, Mie scattering and total extinction at altitude h (km).
// The three sky shaders each wanted a different subset of this; taking the
// superset keeps one copy of the density profile. Callers that want combined
// in-scatter use rayleigh + vec3(mie), which is the same sum the multiscatter
// bake wrote by hand.
void atmosphereSample(float h, out vec3 rayleigh, out float mie, out vec3 extinction)
{
    float rayleighD = exp(-h / RAYLEIGH_H);
    float mieD = exp(-h / MIE_H);
    float ozoneD = max(0.0, 1.0 - abs(h - OZONE_CENTER) / OZONE_WIDTH);
    rayleigh = RAYLEIGH_SCATTER * rayleighD;
    mie = MIE_SCATTER * mieD;
    extinction = RAYLEIGH_SCATTER * rayleighD + vec3(MIE_EXTINCTION * mieD)
                 + OZONE_ABSORB * ozoneD;
}

// Bruneton transmittance UV forward mapping. The inverse lives in the
// transmittance bake shader, which is the only place that needs it.
//
// Uses the clamped far root: inside the atmosphere (r <= Rt) the discriminant
// Rt^2 - r^2(1 - mu^2) is non-negative for every mu, so the OrMiss variant
// would never return -1 here and the two agree over the whole valid domain.
vec2 transmittanceUv(float r, float mu)
{
    float H = sqrt(Rt * Rt - Rg * Rg);
    float rho = sqrt(max(r * r - Rg * Rg, 0.0));
    float d = distanceToTopClamped(r, mu, Rt);
    float d_min = Rt - r;
    float d_max = rho + H;
    return vec2((d - d_min) / (d_max - d_min), rho / H);
}

// Raw LUT fetch with NO ground test -- the multiscatter bake wants this.
// Shaders that must return black below the horizon wrap it themselves; that
// policy genuinely differs between consumers, so it is not baked in here.
vec3 transmittanceLookup(sampler2D lut, float r, float mu)
{
    return texture(lut, transmittanceUv(r, mu)).rgb;
}
