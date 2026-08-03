// Volumetric cloud layer (spec 11.0): a raymarched shell between
// CLOUD_BOTTOM_KM and CLOUD_TOP_KM above the atmosphere's ground radius,
// shaped by the CPU-baked tiling noise fields (sky_clouds.c) and lit by the
// transmittance LUT sun with a dual-lobe HG phase and a powder term.
//
// Everything is in KILOMETERS in a camera-relative planet frame: the ray
// origin sits at altitude obsAltKm on the +Y axis, the planet centre at
// (0, -(Rg + obsAltKm), 0). Keeping the origin at the camera and radii as
// differences is what keeps fp32 alive at horizon distances (Rg is 6360 --
// absolute positions near the shell would eat the whole mantissa).
//
// Returns (in-scattered radiance, transmittance), radiance ABSOLUTE (the
// env cube's convention -- the screen composite applies preExposure, the
// env bake does not). Samplers are parameters so every caller binds its own
// units. All texture fetches are explicit-LOD: the march loop's trip count
// is data-dependent, where implicit derivatives are undefined.
#include "atmosphere.glsl"
#include "sky_lut.glsl"

const float CLOUD_BOTTOM_KM = 1.5;
const float CLOUD_TOP_KM = 4.0;
const float CLOUD_SHAPE_TILE_KM = 8.0;
const float CLOUD_DETAIL_TILE_KM = 1.0;
// Extinction at density 1, per km. Clouds run 10-100/km in the literature;
// 25 reads as a solid cumulus without going coal-black at the base.
const float CLOUD_EXTINCTION = 25.0;
// March-length cap: the horizon path through the shell is ~100 km, but past
// ~40 km aerial extinction has faded clouds into the sky anyway; the cap
// bounds the step size so the horizon does not starve the near field.
const float CLOUD_PATH_CAP_KM = 48.0;
// Stands in for the multiple scattering a single-scatter march cannot
// produce: real cloud tops reflect most of the sun's irradiance while this
// integral captures one bounce of it. Calibrated against the sky's own
// (arbitrary) radiance scale so sunlit faces read brighter than the blue
// sky behind them.
const float CLOUD_MS_GAIN = 8.0;

float cloudHeightFrac(float altKm)
{
    return clamp((altKm - CLOUD_BOTTOM_KM) / (CLOUD_TOP_KM - CLOUD_BOTTOM_KM), 0.0, 1.0);
}

float cloudRemap(float v, float lo, float hi)
{
    return clamp((v - lo) / (hi - lo), 0.0, 1.0);
}

// Density at a planet-frame position. heightFrac is 0 at the shell bottom,
// 1 at the top. detailOn erodes low-density edges with the finer Worley
// field -- the expensive tap, skipped for light marches and the env tier.
float cloudDensity(vec3 posKm, float heightFrac, sampler3D shapeTex, sampler3D detailTex,
                   float coverage, float cloudType, bool detailOn, vec3 windOffsetKm, float lod)
{
    vec3 sp = (posKm + windOffsetKm).xzy / CLOUD_SHAPE_TILE_KM;
    vec4 shape = textureLod(shapeTex, sp, lod);

    // Worley fbm carves the Perlin base so coverage keeps cauliflower edges
    float wfbm = dot(shape.gba, vec3(0.625, 0.25, 0.125));
    float base = cloudRemap(shape.r, wfbm - 1.0, 1.0);

    // Altitude gradient by cloud type: stratus hugs the bottom, cumulus
    // towers. Soft floor so bases are flat-ish, type-driven ceiling.
    float grad = smoothstep(0.0, 0.07, heightFrac) *
                 (1.0 - smoothstep(mix(0.2, 0.7, cloudType), mix(0.3, 1.0, cloudType), heightFrac));

    // The Perlin-Worley field's mass sits in a narrow band (~0.5-0.75), so a
    // raw 1-coverage threshold flips from clear to overcast within a tenth.
    // Mapping coverage onto the band's own extent makes the knob roughly
    // linear in sky fraction.
    float d = cloudRemap(base * grad, mix(0.85, 0.45, clamp(coverage, 0.0, 1.0)), 1.0);

    if (detailOn && d > 0.0 && d < 0.3) {
        vec3 dp = (posKm + windOffsetKm * 1.5).xzy / CLOUD_DETAIL_TILE_KM;
        vec3 det = textureLod(detailTex, dp, lod).rgb;
        float dfbm = dot(det, vec3(0.625, 0.25, 0.125));
        d = cloudRemap(d, dfbm * mix(0.6, 0.2, heightFrac), 1.0);
    }

    return d;
}

// Nearest positive distance along the ray to the sphere of radius R around
// `centre`, or -1. Stable quadratic (the q form): at grazing incidence the
// textbook -b +/- sqrt(disc) cancels catastrophically at Rg scale.
float cloudSphereNear(vec3 ro, vec3 rd, vec3 centre, float R)
{
    vec3 oc = ro - centre;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - R * R;
    float disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    float q = -(b + sign(b) * sqrt(disc));
    float t0 = q;
    float t1 = c / q;
    float tn = min(t0, t1);
    float tf = max(t0, t1);
    return tn > 0.0 ? tn : (tf > 0.0 ? tf : -1.0);
}

vec4 cloud_march(vec3 roKm, vec3 rd, vec3 sunDir, sampler3D shapeTex, sampler3D detailTex,
                 sampler2D transmittanceLut, sampler2D skyViewLut, int steps, int lightSteps,
                 bool detailOn, float coverage, float cloudType, float densityScale,
                 vec3 windOffsetKm, float dither)
{
    float obsAlt = max(roKm.y, VIEW_ALTITUDE);
    // Planet centre Rg+alt straight below the camera: the local flat-earth
    // radial axis, so ro - centre is exactly (0, Rg+obsAlt, 0) and the
    // radial cosine is just rd.y.
    vec3 centre = vec3(roKm.x, roKm.y - (Rg + obsAlt), roKm.z);
    float r0 = Rg + obsAlt;

    // Below-horizon rays belong to the ground (real terrain via the depth
    // test, the virtual floor otherwise) -- never to the shell's far side.
    if (hitsGround(r0, rd.y))
        return vec4(0.0, 0.0, 0.0, 1.0);

    float tIn = cloudSphereNear(roKm, rd, centre, Rg + CLOUD_BOTTOM_KM);
    float tOut = cloudSphereNear(roKm, rd, centre, Rg + CLOUD_TOP_KM);
    if (tOut < 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);
    if (tIn < 0.0)
        tIn = 0.0; // inside the shell already
    if (tIn > tOut) {
        float tmp = tIn;
        tIn = tOut;
        tOut = tmp;
    }

    float pathLen = tOut - tIn;
    // Cap the marched span; fade what the cap cuts off so the horizon does
    // not end in a hard shell edge.
    float tailFade = exp(-max(pathLen - CLOUD_PATH_CAP_KM, 0.0) / 8.0);
    pathLen = min(pathLen, CLOUD_PATH_CAP_KM);
    float dt = pathLen / float(steps);

    float cosVS = dot(rd, sunDir);
    // Dual-lobe Henyey-Greenstein: strong forward silver lining plus a weak
    // backscatter lobe so the anti-solar side is not dead flat.
    float phase = mix(phaseHG(cosVS, 0.85), phaseHG(cosVS, -0.3), 0.5);

    // Ambient: one zenith sky tap, hoisted -- the shell is lit by the whole
    // sky dome and per-sample directional detail is below this term's noise.
    // The pi converts the radiance tap into an irradiance-ish fill; without
    // it shadow sides render charcoal, not cloud-grey.
    vec3 ambient =
        textureLod(skyViewLut, skyViewUv(vec3(0.0, 1.0, 0.0), sunDir, r0), 0.0).rgb * PI;

    // Short exponential cone toward the sun (total ~1.2 km): resolves the
    // self-shadowed base without letting one far tap through a neighbouring
    // tower account for kilometres of extinction and blacken everything.
    float lightOff[6] = float[6](0.03, 0.07, 0.15, 0.3, 0.6, 1.2);

    vec3 S = vec3(0.0);
    float T = 1.0;

    for (int i = 0; i < steps; i++) {
        float t = tIn + (float(i) + dither) * dt;
        vec3 pos = roKm + rd * t;
        float alt = length(pos - centre) - Rg;
        float hf = cloudHeightFrac(alt);

        float d = cloudDensity(pos, hf, shapeTex, detailTex, coverage, cloudType, detailOn,
                               windOffsetKm, 0.0);
        if (d <= 0.0)
            continue;

        float sigma = d * CLOUD_EXTINCTION * densityScale;

        // Optical depth toward the sun, density-only taps at a coarser mip
        float tauL = 0.0;
        float prevOff = 0.0;
        for (int l = 0; l < lightSteps; l++) {
            float off = lightOff[l];
            vec3 lp = pos + sunDir * off;
            float lhf = cloudHeightFrac(length(lp - centre) - Rg);
            float ld = cloudDensity(lp, lhf, shapeTex, detailTex, coverage, cloudType, false,
                                    windOffsetKm, 1.0);
            tauL += ld * CLOUD_EXTINCTION * densityScale * (off - prevOff);
            prevOff = off;
        }

        // Sun color at the sample's altitude through the atmosphere, then
        // through the cloud's own optical depth. The dual Beer is the
        // standard multiple-scattering cheat: light that scattered forward a
        // few times still arrives, so deep cloud reads luminous grey instead
        // of coal. Powder darkens crevices but is floored so thin fringes
        // (tauL ~ 0) stay lit.
        float rS = Rg + clamp(alt, 0.0, CLOUD_TOP_KM);
        vec3 sunT = transmittanceToSky(transmittanceLut, rS, sunDir.y);
        float beer = max(exp(-tauL), exp(-tauL * 0.25) * 0.7);
        float powder = 1.0 - 0.6 * exp(-2.0 * tauL);
        vec3 Lsun = SUN_ILLUMINANCE * sunT * beer * phase * powder * CLOUD_MS_GAIN;
        vec3 Ls = Lsun + ambient * 1.5 * mix(0.4, 1.0, hf);

        // Energy-conserving per-step integration (Hillaire): the analytic
        // integral of T over the step, not a rectangle rule.
        float stepT = exp(-sigma * dt);
        S += T * Ls * (1.0 - stepT);
        T *= stepT;
        if (T < 0.01)
            break;
    }

    // The tail fade applies to what the march produced: beyond the cap the
    // shell blends toward clear sky rather than clipping.
    S *= tailFade;
    T = mix(1.0, T, tailFade);

    return vec4(S, T);
}
