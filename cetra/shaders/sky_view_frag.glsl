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
// The night-sky floor (spec 11.80): airglow + zodiacal light + integrated
// starlight, premultiplied on the CPU (colour x base x brightness x the
// civil-twilight ramp; vec3(0) = off, which adds a literal zero). Living in
// THIS bake is the design: everything that needs the floor -- the
// background, the env cube and so the whole IBL, the cloud march's ambient
// -- already samples this LUT, and the LUT re-bakes exactly when the ramp's
// one input (the sun) moves.
uniform vec3 nightFloor;

#include "sky_lut.glsl"

// MIE_G, rayleighPhase, miePhase and multiscatterAt live in atmosphere.glsl:
// the aerial-perspective volume marches the same medium and must not evaluate
// a second, drifting copy of it.

const int SKY_STEPS = 32;

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
    // No miss case: the observer is inside the atmosphere, so both roots
    // exist for every mu (see atmosphere.glsl).
    float tMax = ground ? distanceToGround(r, mu) : distanceToTop(r, mu);
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

        Atmosphere atm = atmosphereAt(rt - Rg);

        vec3 stepTrans = exp(-atm.extinction * dt);
        vec3 sunT = transmittanceToSky(transmittanceLut, rt, mu_s);

        // Single scattering: sun light * phase-weighted scattering
        vec3 single = sunT * (atm.rayleigh * phaseR + vec3(atm.mie * phaseM));
        // Multiple scattering: isotropic Psi * total scattering
        vec3 multi = multiscatterAt(multiscatterLut, rt, mu_s) * (atm.rayleigh + vec3(atm.mie));

        vec3 scatterIntegral =
            (single + multi) * (vec3(1.0) - stepTrans) / max(atm.extinction, vec3(1e-6));
        L += through * scatterIntegral;
        through *= stepTrans;
    }

    // Scale to the engine's linear range; keep HDR (bloom uses it) but
    // bound against fp16 overflow
    vec3 sky = min(L * SUN_ILLUMINANCE, vec3(100.0));
    // The floor sits above the atmosphere's bulk, so `through` -- this ray's
    // own transmittance to the top -- dims it toward the horizon with no new
    // lookup. The TINT rides at partial saturation, the stars' rule: full
    // spectral extinction painted the whole lower sky a muddy brown at any
    // radiance worth having. A ground-hitting ray must not glow.
    if (!ground) {
        vec3 t = mix(vec3(dot(through, vec3(0.2126, 0.7152, 0.0722))), through, 0.35);
        sky += nightFloor * t;
    }
    FragColor = vec4(sky, 1.0);
}
