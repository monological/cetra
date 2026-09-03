#version 330 core
in vec2 TexCoords;
out vec4 FragColor; // rgb = in-scatter to add, a = transmittance to multiply by

// Aerial perspective volume (Hillaire 2016, spec 9.6): the atmosphere between
// the camera and every point in the view frustum, so distant geometry fades
// into the sky's OWN colour instead of a flat fog tint.
//
// One draw per slice into a 3D texture. Each cell marches from the camera to
// its own depth against the same transmittance + multiscatter LUTs the sky-view
// LUT was baked from -- via the shared atmosphere.glsl, so there is no second
// copy of the medium to drift from the one the LUTs encode.
//
// The observer sits at the sky's fixed VIEW_ALTITUDE rather than the camera's
// world height. That is deliberate: it makes this volume and the sky-view LUT
// agree about the air they share, so a far ridge converges on the colour of the
// sky immediately above it instead of meeting it at a seam.

uniform sampler2D transmittanceLut;
uniform sampler2D multiscatterLut;
uniform vec3 sunDir;     // world space, unit vector TOWARD the sun
uniform mat4 invView;    // view -> world
uniform mat4 projection; // read by depth.glsl
uniform float aerialFar; // far depth of the volume, WORLD units
uniform float unitsPerKm;
uniform int aerialDepth; // slice count; mirrors SKY_AERIAL_Z
uniform int sliceIndex;

#include "atmosphere.glsl"
#include "froxel.glsl"

// 16 is enough because the integrand is smooth over a single cell's span -- the
// exponential slicing already puts the short, rapidly-varying steps near the
// camera where the density gradient is steepest.
const int AERIAL_STEPS = 16;

void main() {
    float nearZ = nearPlaneDist();

    // Depth mapping is shared with the fog volume (include/froxel.glsl), in
    // WORLD units. The jitter of 1.0 is load-bearing: it integrates to the FAR
    // FACE of the slice, which is the convention froxel_integrate uses, so the
    // composite indexes both volumes with the same expression instead of
    // carrying two off-by-half-a-slice rules.
    vec3 viewPos = froxelViewPos(TexCoords, float(sliceIndex), 1.0, nearZ, aerialFar,
                                 float(aerialDepth), 1.0);
    vec3 camPos = invView[3].xyz;
    vec3 toCell = (invView * vec4(viewPos, 1.0)).xyz - camPos;
    float distKm = length(toCell) / unitsPerKm;
    vec3 rd = normalize(toCell);

    // Planet-centred frame: world +Y is up, matching the sky's own convention.
    vec3 ro = vec3(0.0, Rg + VIEW_ALTITUDE, 0.0);

    float cosVS = dot(rd, sunDir);
    float phaseR = rayleighPhase(cosVS);
    float phaseM = miePhase(cosVS);

    vec3 L = vec3(0.0);
    vec3 T = vec3(1.0);
    float dt = distKm / float(AERIAL_STEPS);

    for (int i = 0; i < AERIAL_STEPS; i++) {
        vec3 p = ro + rd * (dt * (float(i) + 0.5));
        float r = max(length(p), Rg);
        Atmosphere atm = atmosphereAt(r - Rg);
        float mu_s = dot(p / r, sunDir);

        vec3 sunT = transmittanceToSky(transmittanceLut, r, mu_s);
        vec3 single = sunT * (atm.rayleigh * phaseR + vec3(atm.mie * phaseM));
        // Psi is stored per unit sun illuminance (see sky_multiscatter_frag), so
        // it is scaled by the same SUN_ILLUMINANCE as the single-scatter term --
        // applied once to the sum below, exactly as sky_view_frag does it.
        // Scaling only one of the two silently dims multiple scattering, which
        // makes distant surfaces diverge from the sky drawn behind them.
        vec3 multi = multiscatterAt(multiscatterLut, r, mu_s) * (atm.rayleigh + vec3(atm.mie));

        // Energy-conserving step, the same form froxel_integrate uses: the
        // analytic integral of in-scatter over a segment of constant medium,
        // rather than a midpoint sample scaled by dt. Epsilon matches
        // sky_view_frag so the two marches cannot diverge at zero extinction.
        vec3 stepT = exp(-atm.extinction * dt);
        L += T * (single + multi) / max(atm.extinction, vec3(1e-6)) * (vec3(1.0) - stepT);
        T *= stepT;
    }
    // Scaled once at the end, and bounded against fp16 overflow on the way into
    // an RGBA16F volume -- both exactly as sky_view_frag does it.
    L = min(L * SUN_ILLUMINANCE, vec3(100.0));

    // Transmittance collapses to a scalar because the composite folds this in
    // with glBlendFunc(GL_ONE, GL_SRC_ALPHA) -- one factor for the whole scene
    // colour, so per-channel extinction cannot survive the blend. The reference
    // implementation makes the same trade: the colour of distance comes
    // overwhelmingly from the in-scatter that is ADDED, not from the
    // wavelength-dependence of what is removed.
    FragColor = vec4(L, dot(T, vec3(0.2126, 0.7152, 0.0722)));
}
