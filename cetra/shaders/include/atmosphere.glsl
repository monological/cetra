// Hillaire (EGSR 2020) / Bruneton (2017) atmosphere model: the constants and
// the geometry every sky_* shader shares. ONE copy -- the transmittance LUT
// bake and its four consumers must integrate the same atmosphere or the sky is
// energetically wrong in a way that still looks plausible.
//
// All distances are in KILOMETERS: the planetary magnitudes (Rt ~ 6460) stay
// float-safe and the scattering coefficients below are per-km.
//
// NOTE: sky.c keeps an independent C mirror of this whole constant set (the
// SKY_* defines and sky_medium_at, the C analogue of atmosphereAt below) for
// the CPU-side sun transmittance and sky ambient, since there is no GPU
// readback. Changing a value here without changing that one silently desyncs
// the analytic key light's colour and intensity, and the fog's ambient
// in-scatter, from the sky they stand under. Named rather than cited by line
// so the reference cannot rot.

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

// Ray/sphere intersection. The sky shaders had forked this into two bodies
// under three names -- some clamping the discriminant to zero, some returning
// -1.0 to signal a miss -- and an earlier pass at this chunk preserved BOTH
// contracts on the assumption that callers depended on the difference. They do
// not: every live call site is provably inside the atmosphere, so the
// discriminant is never negative and the miss path is unreachable.
//
//   Ground (near root): only ever called under hitsGround(), which tests the
//   SAME discriminant with the SAME R, so disc >= 0 by construction.
//   Top (far root): the observer sits at r = Rg + VIEW_ALTITUDE = 6360.5 < Rt,
//   so disc = Rt^2 - r^2(1 - mu^2) >= Rt^2 - r^2 = 1.28e6 > 0 for every mu.
//
// So one contract each. If a caller ever sits OUTSIDE the atmosphere (r > Rt)
// the miss case becomes real and needs reintroducing -- deliberately, with
// that reason stated, rather than because two copies once disagreed.

// Far root: exit through sphere R, for an observer inside it.
float distanceToTop(float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

// Near root: first hit ahead of the ray. Used for the ground so a march stops
// at the surface instead of continuing through the planet to the far root,
// which produced huge/NaN samples.
float distanceToGround(float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + Rg * Rg;
    return max(0.0, -r * mu - sqrt(max(disc, 0.0)));
}

bool hitsGround(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + Rg * Rg >= 0.0;
}

// The density profile at altitude h (km). The three sky shaders each want a
// different subset -- extinction alone, combined in-scatter, or the Rayleigh
// and Mie terms separated for the phase functions -- so this returns all
// three and each caller reads what it needs. Returning a struct rather than
// three out-params matters: the out-param form made a caller that wanted one
// value declare three locals, which is why two shaders had grown wrapper
// functions purely to hide the ceremony.
struct Atmosphere {
    vec3 rayleigh;   // Rayleigh scattering coefficient
    float mie;       // Mie scattering coefficient
    vec3 extinction; // total extinction (Rayleigh + Mie + ozone)
};

Atmosphere atmosphereAt(float h)
{
    float rayleighD = exp(-h / RAYLEIGH_H);
    float mieD = exp(-h / MIE_H);
    float ozoneD = max(0.0, 1.0 - abs(h - OZONE_CENTER) / OZONE_WIDTH);
    return Atmosphere(RAYLEIGH_SCATTER * rayleighD,
                      MIE_SCATTER * mieD,
                      RAYLEIGH_SCATTER * rayleighD + vec3(MIE_EXTINCTION * mieD)
                          + OZONE_ABSORB * ozoneD);
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
    float d = distanceToTop(r, mu);
    float d_min = Rt - r;
    float d_max = rho + H;
    return vec2((d - d_min) / (d_max - d_min), rho / H);
}

// Raw LUT fetch with NO ground test. Only the multiscatter bake's ground-bounce
// term wants this -- it has already established mu > 0, so the horizon test
// would be redundant there.
vec3 transmittanceLookup(sampler2D lut, float r, float mu)
{
    return texture(lut, transmittanceUv(r, mu)).rgb;
}

// Transmittance with the below-horizon cut: black under the ground. This is
// what every sun-facing lookup wants, and it lives here rather than in a
// caller-side wrapper because four sibling shaders had each grown their own
// `transmittanceTo` -- three meaning THIS, one meaning the raw lookup above.
// One name, one meaning.
vec3 transmittanceToSky(sampler2D lut, float r, float mu)
{
    if (hitsGround(r, mu))
        return vec3(0.0);
    return transmittanceLookup(lut, r, mu);
}

// Mie asymmetry. Part of the medium, so it belongs with the coefficients above
// rather than with any one consumer.
const float MIE_G = 0.8;

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

// Psi LUT fetch: the multiple-scattering contribution at altitude r for a sun
// at cos-zenith mu_s. Takes the sampler as a parameter, matching the
// transmittance helpers above -- a shader binds its own uniform and passes it,
// so nothing depends on a particular uniform name.
vec3 multiscatterAt(sampler2D lut, float r, float mu_s)
{
    vec2 uv = vec2(mu_s * 0.5 + 0.5, (r - Rg) / (Rt - Rg));
    return texture(lut, uv).rgb;
}
